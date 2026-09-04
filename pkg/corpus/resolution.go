// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/resolution.go — Go port of vmaftune.resolution.
//
// The fork ships two production-grade pooled-mean VMAF models: vmaf_v0.6.1
// (1080p viewing setup) and vmaf_4k_v0.6.1 (4K display setup). The wrong model
// on the wrong resolution biases scores by several VMAF points, so the corpus
// sweep selects per rung:
//
//	height >= 2160  -> 4K model
//	otherwise       -> 1080p model (the fork has no 720p / SD model; 1080p is
//	                   the canonical fallback and matches Netflix's guidance).

package corpus

import (
	"fmt"
	"github.com/VMAFx/vmafx/pkg/model"
	"strings"
)

// height4KThreshold is the line count at and above which the 4K model is
// selected. 2160 is the UHD-1 standard.
const height4KThreshold = 2160

// Model identifiers mirror libvmaf's "--model version=" vocabulary so the
// strings flow straight through ScoreRequest.Model.
const (
	// Model1080P follows the fork default (ADR-1168); Model4K is its v1
	// counterpart for 2160p, which docs/models/v1.md names the 4K default
	// (2160p viewed at 1.5H). Both are vmaf_v1.0.16.
	//
	// The NEG entries stay on the v0.6.1 family on purpose: Netflix published
	// no NEG counterpart to any vmaf_v1.0.16_* model, so there is nothing to
	// point them at. Requesting NEG therefore also changes model generation —
	// documented in docs/metrics/vmaf-neg.md — until a v1 NEG model exists.
	Model1080P = model.DefaultVersion
	Model4K    = "vmaf_v1.0.16_1d5h_2160"

	// NEG (No Enhancement Gain) variants resist sharpening-based score
	// inflation. Use for codec A-vs-B comparisons, not production
	// monitoring. See docs/metrics/vmaf-neg.md.
	Model1080PNEG = model.DefaultNEGVersion
	Model4KNEG    = "vmaf_4k_v0.6.1neg"
)

// SelectVMAFModelVersion returns the libvmaf "--model version=" string for an
// encode. width is accepted for API symmetry with future anamorphic-aware
// extensions; the current rule is height-only.
func SelectVMAFModelVersion(width, height int) (string, error) {
	if width <= 0 || height <= 0 {
		return "", fmt.Errorf("resolution must be positive (got width=%d, height=%d)",
			width, height)
	}
	if height >= height4KThreshold {
		return Model4K, nil
	}
	return Model1080P, nil
}

// NegModelFor returns the NEG variant of a standard VMAF model version string.
//
// Pre-formatted "key=value" overrides (path= / version=) pass through
// unchanged — they are model-path overrides, not version identifiers. Strings
// already ending in "neg" are returned as-is, so the function is idempotent.
// Unknown models get a "neg" suffix appended so libvmaf surfaces a clear
// missing-model error rather than silently using the wrong model.
func NegModelFor(modelVersion string) string {
	if strings.Contains(modelVersion, "=") {
		return modelVersion
	}
	if strings.HasSuffix(modelVersion, "neg") {
		return modelVersion
	}
	switch modelVersion {
	case Model1080P:
		return Model1080PNEG
	case Model4K:
		return Model4KNEG
	}
	return modelVersion + "neg"
}

// CRFOffsetForResolution returns the CRF offset for an encode resolution.
//
// 1080p is the baseline (offset 0). Higher resolutions get a negative offset
// (lower CRF, more bits) because the same nominal CRF on 4K under-shoots a
// flat-VMAF target compared to 1080p under typical x264 / x265 / AV1 RDO; lower
// resolutions get a positive offset.
func CRFOffsetForResolution(width, height int) (int, error) {
	if width <= 0 || height <= 0 {
		return 0, fmt.Errorf("resolution must be positive (got width=%d, height=%d)",
			width, height)
	}
	switch {
	case height >= height4KThreshold:
		return -2, nil
	case height >= 1080:
		return 0, nil
	case height >= 720:
		return 2, nil
	default:
		return 4, nil
	}
}
