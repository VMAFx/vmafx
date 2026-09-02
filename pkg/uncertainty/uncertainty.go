// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package uncertainty is the Go port of tools/vmaf-tune/src/vmaftune/
// uncertainty.py — the shared confidence-band helpers the uncertainty-aware
// recipes in `recommend`, `ladder` and `auto` all consume.
//
// The conformal-VQA surface (ADR-0279) turns the predictor's verdict from a
// binary GOSPEL / FALL_BACK into a continuous (point, low, high) interval.
// Two width thresholds carve that interval into three bands:
//
//   - width <= tight  — the predictor is confident; recipes may trust the
//     point estimate and short-circuit their search.
//   - width >= wide   — the predictor is uncertain; recipes widen their
//     search range or insert extra ladder rungs.
//   - between         — defer to the native non-uncertainty recipe, so
//     behaviour is byte-identical to the pre-uncertainty code path.
//
// Threshold provenance: the 2.0 / 5.0 VMAF defaults come from Research-0067
// (Phase F decision tree) — 2 VMAF is roughly a JND on a smoothed corpus, so
// a narrower interval is empirically indistinguishable from the point
// estimate; 5 VMAF spans more than one ABR rung, so a wider interval means
// the predictor cannot localise even the rung.
//
// This package only affects search cost and which row gets picked from an
// equivalence class of qualifying rows. It does NOT widen the production-flip
// gate, which lives in pkg/predictor's validation harness.
package uncertainty

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"math"
	"os"
)

// Documented defaults from Research-0067 (Phase F feasibility study).
const (
	DefaultTightIntervalMaxWidth = 2.0
	DefaultWideIntervalMinWidth  = 5.0
)

// Decision is the per-call recipe-override band returned by Classify.
type Decision string

const (
	// Tight — the predictor is confident enough to short-circuit search or
	// hold a ladder rung as authoritative.
	Tight Decision = "tight"
	// Middle — defer to the native point-estimate recipe; preserves the
	// pre-uncertainty behaviour exactly.
	Middle Decision = "middle"
	// Wide — the predictor is uncertain; widen search / add rungs.
	Wide Decision = "wide"
)

// Thresholds carries the corpus-derived (tight, wide) width pair plus a
// provenance string for the JSON metadata block downstream recipes emit.
type Thresholds struct {
	TightIntervalMaxWidth float64
	WideIntervalMinWidth  float64
	Source                string
}

// DefaultThresholds returns the Research-0067 emergency floor.
func DefaultThresholds() Thresholds {
	return Thresholds{
		TightIntervalMaxWidth: DefaultTightIntervalMaxWidth,
		WideIntervalMinWidth:  DefaultWideIntervalMinWidth,
		Source:                "default",
	}
}

// Validate enforces 0 < tight <= wide so a malformed sidecar fails fast
// rather than silently producing nonsense decisions.
func (t Thresholds) Validate() error {
	if !(t.TightIntervalMaxWidth > 0.0 && t.WideIntervalMinWidth > 0.0) {
		return fmt.Errorf(
			"uncertainty: both widths must be positive; got tight=%v, wide=%v",
			t.TightIntervalMaxWidth, t.WideIntervalMinWidth)
	}
	if t.TightIntervalMaxWidth > t.WideIntervalMinWidth {
		return fmt.Errorf(
			"uncertainty: tight_interval_max_width must be <= "+
				"wide_interval_min_width; got tight=%v, wide=%v",
			t.TightIntervalMaxWidth, t.WideIntervalMinWidth)
	}
	return nil
}

// sidecarDoc is the calibration-sidecar schema. Extra keys are ignored so the
// loader survives schema growth.
type sidecarDoc struct {
	TightIntervalMaxWidth *float64 `json:"tight_interval_max_width"`
	WideIntervalMinWidth  *float64 `json:"wide_interval_min_width"`
}

// LoadThresholds reads corpus-derived thresholds from a calibration sidecar.
//
// It never fails: an absent path, an unreadable file, malformed JSON, missing
// keys or a violated tight<=wide invariant each log one warning and return
// the documented defaults. A missing sidecar must degrade the recipe, not
// kill the run.
func LoadThresholds(sidecarPath string, log *slog.Logger) Thresholds {
	if log == nil {
		log = slog.Default()
	}
	fallback := DefaultThresholds()

	if sidecarPath == "" {
		log.Warn("vmaf-tune uncertainty: no calibration sidecar provided; "+
			"falling back to documented defaults",
			"tight", DefaultTightIntervalMaxWidth,
			"wide", DefaultWideIntervalMinWidth)
		return fallback
	}
	// #nosec G304 -- sidecarPath comes from an operator-supplied CLI flag.
	data, err := os.ReadFile(sidecarPath)
	if err != nil {
		log.Warn("vmaf-tune uncertainty: calibration sidecar unreadable; "+
			"falling back to documented defaults",
			"path", sidecarPath, "error", err,
			"tight", DefaultTightIntervalMaxWidth,
			"wide", DefaultWideIntervalMinWidth)
		return fallback
	}
	var doc sidecarDoc
	if unmarshalErr := json.Unmarshal(data, &doc); unmarshalErr != nil ||
		doc.TightIntervalMaxWidth == nil || doc.WideIntervalMinWidth == nil {
		log.Warn("vmaf-tune uncertainty: calibration sidecar malformed; "+
			"falling back to documented defaults",
			"path", sidecarPath, "error", unmarshalErr,
			"tight", DefaultTightIntervalMaxWidth,
			"wide", DefaultWideIntervalMinWidth)
		return fallback
	}
	candidate := Thresholds{
		TightIntervalMaxWidth: *doc.TightIntervalMaxWidth,
		WideIntervalMinWidth:  *doc.WideIntervalMinWidth,
		Source:                sidecarPath,
	}
	if validateErr := candidate.Validate(); validateErr != nil {
		log.Warn("vmaf-tune uncertainty: calibration sidecar violates the "+
			"tight<=wide invariant; falling back to documented defaults",
			"path", sidecarPath, "error", validateErr)
		return fallback
	}
	return candidate
}

// Classify carves one interval width into its confidence band.
//
// A NaN width (an uncalibrated predictor) returns Middle so the caller falls
// back to the native non-uncertainty recipe — exactly what the "no
// calibration shipped" path needs. A negative width is a caller bug (conformal
// widths are non-negative by construction) and returns an error.
func Classify(width float64, t Thresholds) (Decision, error) {
	if math.IsNaN(width) {
		return Middle, nil
	}
	if width < 0.0 {
		return "", fmt.Errorf(
			"uncertainty: interval width must be >= 0.0 or NaN; got %v", width)
	}
	switch {
	case width <= t.TightIntervalMaxWidth:
		return Tight, nil
	case width >= t.WideIntervalMinWidth:
		return Wide, nil
	default:
		return Middle, nil
	}
}

// ExcludesTarget reports whether the whole prediction interval lies strictly
// above or below target, with slack VMAF of margin.
//
// Used by `recommend` to short-circuit the CRF search once the predictor has
// already determined that no CRF in the current bracket can hit the target.
// A non-finite bound is never conclusive and returns false.
func ExcludesTarget(low, high, target, slack float64) bool {
	if math.IsNaN(low) || math.IsInf(low, 0) || math.IsNaN(high) || math.IsInf(high, 0) {
		return false
	}
	return high < target-slack || low > target+slack
}
