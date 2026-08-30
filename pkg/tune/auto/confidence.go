// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package auto

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"math"
	"os"
)

// F.3 confidence-aware fallback thresholds.
//
// F.2 treats the predictor's verdict as a binary GOSPEL / FALL_BACK gate. F.3
// makes the gate continuous by consulting the conformal interval half-width
// (ADR-0279). The two thresholds carve the width axis into three regions:
//
//   - width <= tight  → predictor is confident; trust the point estimate even
//     if the native verdict was nominally FALL_BACK.
//   - width >= wide   → predictor is uncertain; force escalation to the
//     coarse-to-fine search even if the native verdict was GOSPEL.
//   - between         → defer to the native verdict (F.2 behaviour).
//
// The defaults are documented in Research-0067 and act as an emergency floor
// when no corpus-derived calibration sidecar ships with the model.
const (
	DefaultTightIntervalMaxWidth = 2.0
	DefaultWideIntervalMinWidth  = 5.0
)

// ConfidenceDecision is the outcome of the per-cell escalation policy.
type ConfidenceDecision string

const (
	// SkipEscalation — the predictor is confident enough that a FALL_BACK
	// verdict is overridden; trust the point estimate.
	SkipEscalation ConfidenceDecision = "skip-escalation"
	// RecommendEscalation — the interval width is in the middle band; defer
	// to the native verdict.
	RecommendEscalation ConfidenceDecision = "recommend-escalation"
	// ForceEscalation — the predictor is uncertain enough that a GOSPEL
	// verdict is overridden; escalate anyway.
	ForceEscalation ConfidenceDecision = "force-escalation"
)

// ConfidenceThresholds are the width gates carved from the calibration
// corpus. A valid pair satisfies 0 < tight <= wide; NewConfidenceThresholds
// enforces that so a malformed sidecar fails fast instead of silently
// producing nonsense decisions.
type ConfidenceThresholds struct {
	TightIntervalMaxWidth float64
	WideIntervalMinWidth  float64
	// Source records where the values came from, for the JSON metadata block.
	Source string
}

// DefaultConfidenceThresholds returns the documented emergency floor.
func DefaultConfidenceThresholds() ConfidenceThresholds {
	return ConfidenceThresholds{
		TightIntervalMaxWidth: DefaultTightIntervalMaxWidth,
		WideIntervalMinWidth:  DefaultWideIntervalMinWidth,
		Source:                "default",
	}
}

// NewConfidenceThresholds validates and constructs a threshold pair.
func NewConfidenceThresholds(tight, wide float64, source string) (ConfidenceThresholds, error) {
	if !(tight > 0.0 && wide > 0.0) {
		return ConfidenceThresholds{}, fmt.Errorf(
			"ConfidenceThresholds: both widths must be positive; got tight=%v, wide=%v",
			tight, wide)
	}
	if tight > wide {
		return ConfidenceThresholds{}, fmt.Errorf(
			"ConfidenceThresholds: tight_interval_max_width must be <= "+
				"wide_interval_min_width; got tight=%v, wide=%v", tight, wide)
	}
	return ConfidenceThresholds{
		TightIntervalMaxWidth: tight,
		WideIntervalMinWidth:  wide,
		Source:                source,
	}, nil
}

// LoadConfidenceThresholds reads corpus-derived thresholds from a calibration
// sidecar produced by the conformal-VQA pipeline.
//
// Extra keys are ignored so the loader survives schema growth. Every failure
// path — empty path, missing file, malformed JSON, missing key, invalid
// ordering — falls back to the documented defaults with a one-line warning.
// Per the no-test-weakening rule the defaults are the *floor* surface: they
// keep the gate functional while signalling that the corpus fit has not
// landed.
func LoadConfidenceThresholds(sidecarPath string, log *slog.Logger) ConfidenceThresholds {
	if log == nil {
		log = slog.Default()
	}
	if sidecarPath == "" {
		log.Warn("vmaf-tune auto F.3: no calibration sidecar provided; "+
			"falling back to documented defaults",
			"tight", DefaultTightIntervalMaxWidth, "wide", DefaultWideIntervalMinWidth)
		return DefaultConfidenceThresholds()
	}
	data, err := os.ReadFile(sidecarPath) // #nosec G304 -- operator-supplied CLI flag
	if err != nil {
		log.Warn("vmaf-tune auto F.3: calibration sidecar unreadable; "+
			"falling back to documented defaults", "path", sidecarPath, "error", err)
		return DefaultConfidenceThresholds()
	}
	var doc struct {
		Tight *float64 `json:"tight_interval_max_width"`
		Wide  *float64 `json:"wide_interval_min_width"`
	}
	if err := json.Unmarshal(data, &doc); err != nil || doc.Tight == nil || doc.Wide == nil {
		log.Warn("vmaf-tune auto F.3: calibration sidecar missing required keys; "+
			"falling back to documented defaults", "path", sidecarPath)
		return DefaultConfidenceThresholds()
	}
	thresholds, err := NewConfidenceThresholds(*doc.Tight, *doc.Wide, sidecarPath)
	if err != nil {
		log.Warn("vmaf-tune auto F.3: calibration sidecar has invalid thresholds; "+
			"falling back to documented defaults", "path", sidecarPath, "error", err)
		return DefaultConfidenceThresholds()
	}
	return thresholds
}

// ConfidenceAwareEscalation decides per-cell escalation from the native
// verdict plus the conformal interval width.
//
// Decision table:
//
//	width is NaN               → RecommendEscalation (uncalibrated: no override)
//	width <= tight             → SkipEscalation      (override)
//	width >= wide              → ForceEscalation     (override)
//	tight < width < wide       → defer to verdict:
//	    "FALL_BACK" or ""      → RecommendEscalation
//	    anything else          → SkipEscalation
//
// The native verdict is honoured in the middle band, which preserves F.2's
// gate exactly when the predictor is neither confident nor uncertain. The
// override branches are the only places F.3 disagrees with F.2.
//
// A negative width is a caller bug (the conformal interval is high − low) and
// is reported as an error rather than silently bucketed.
func ConfidenceAwareEscalation(
	verdict string,
	intervalWidth float64,
	thresholds ConfidenceThresholds,
) (ConfidenceDecision, error) {
	if math.IsNaN(intervalWidth) {
		return RecommendEscalation, nil
	}
	if intervalWidth < 0.0 {
		return "", fmt.Errorf(
			"ConfidenceAwareEscalation: interval_width must be >= 0.0 or NaN; got %v",
			intervalWidth)
	}
	if intervalWidth <= thresholds.TightIntervalMaxWidth {
		return SkipEscalation, nil
	}
	if intervalWidth >= thresholds.WideIntervalMinWidth {
		return ForceEscalation, nil
	}
	if verdict == VerdictFallBack || verdict == "" {
		return RecommendEscalation, nil
	}
	return SkipEscalation, nil
}
