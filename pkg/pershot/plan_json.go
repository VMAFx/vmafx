// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// pkg/pershot/plan_json.go — per-shot plan JSON emitter.
//
// Byte-compatible port of the plan_doc block in
// tools/vmaf-tune/src/vmaftune/cli.py _run_tune_per_shot, which renders with
// json.dumps(..., indent=2, sort_keys=True).
//
// Two Python behaviours have to be reproduced deliberately, because Go's
// encoding/json does neither by default:
//
//  1. sort_keys=True. Go emits struct fields in declaration order, so every
//     wire struct below declares its fields in alphabetical key order. Do not
//     reorder them.
//  2. repr()-style floats. Python renders 24.0 as "24.0"; Go's shortest-
//     round-trip formatter renders it as "24". pyFloat restores the trailing
//     ".0" and reproduces Python repr's fixed-vs-exponent threshold, so a
//     plan emitted by either implementation is byte-identical.

package pershot

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

// pyFloat is a float64 that marshals the way Python's json.dumps does.
type pyFloat float64

// MarshalJSON renders the value with Python repr() semantics.
//
// CPython's float_repr_style uses fixed-point notation for
// 1e-4 <= |v| < 1e16 and exponent notation outside it, always keeping at
// least one fractional digit. Go's strconv 'f'/'g' verbs with precision -1
// produce the same shortest round-trip digits, so matching the notation
// choice and re-adding the trailing ".0" is sufficient.
//
// Non-finite values are rejected rather than emitted: JSON has no NaN or
// Infinity literal (RFC 8259), and the Python emitter maps them to null via
// _shot_bitrate before they reach json.dumps. A non-finite value arriving
// here means a caller skipped that mapping, and a hard error is better than
// a document no strict parser will read.
func (v pyFloat) MarshalJSON() ([]byte, error) {
	f := float64(v)
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return nil, fmt.Errorf("pershot: refusing to serialise non-finite float %v", f)
	}
	return []byte(formatPyFloat(f)), nil
}

// formatPyFloat renders f the way Python's repr() would.
func formatPyFloat(f float64) string {
	abs := math.Abs(f)
	var s string
	if f == 0 || (abs >= 1e-4 && abs < 1e16) {
		s = strconv.FormatFloat(f, 'f', -1, 64)
	} else {
		s = strconv.FormatFloat(f, 'g', -1, 64)
		// Python renders exponents with at least two digits ("1e-05", not
		// "1e-5"); Go's 'g' already does the same, so only the mantissa
		// needs the ".0" treatment below.
	}
	for _, c := range s {
		if c == '.' || c == 'e' || c == 'E' {
			return s
		}
	}
	return s + ".0"
}

// shotWire is one entry of the plan's "shots" array. Field order is the
// alphabetical key order Python's sort_keys=True produces.
type shotWire struct {
	BitratekBps   *pyFloat `json:"bitrate_kbps"`
	CRF           int      `json:"crf"`
	EndFrame      int      `json:"end_frame"`
	PredictedVMAF pyFloat  `json:"predicted_vmaf"`
	StartFrame    int      `json:"start_frame"`
}

// planWire is the top-level plan document. Field order is the alphabetical
// key order Python's sort_keys=True produces.
type planWire struct {
	ConcatCommand   []string   `json:"concat_command"`
	Encoder         string     `json:"encoder"`
	Framerate       pyFloat    `json:"framerate"`
	Predicate       string     `json:"predicate"`
	SegmentCommands [][]string `json:"segment_commands"`
	Shots           []shotWire `json:"shots"`
	TargetVMAF      pyFloat    `json:"target_vmaf"`
}

// shotBitrate rounds a measured bitrate to two decimals, or returns nil for
// a non-finite value.
//
// Mirrors cli._shot_bitrate: NaN / Inf serialise as JSON null (RFC 8259
// portability, ADR-0531), which the report ingester maps back to NaN and
// renders as an em dash — the correct rendering for a synthetic or dry-run
// predicate that never performed a real encode.
func shotBitrate(br float64) *pyFloat {
	if math.IsNaN(br) || math.IsInf(br, 0) {
		return nil
	}
	// RoundToEven matches Python's round(), which breaks exact .5 ties to
	// the nearest even value rather than always rounding away from zero.
	rounded := pyFloat(math.RoundToEven(br*100) / 100)
	return &rounded
}

// RenderPlanJSON serialises an EncodingPlan as the per-shot plan document.
//
// predicateLabel names the selector that produced the recommendations
// ("bisect" for the production path, or the operator's own identifier).
// targetVMAF is echoed back so a consumer of the plan can see what it was
// tuned against.
//
// The returned string carries no trailing newline; callers append one, as
// the Python does when writing to stdout or a file.
func RenderPlanJSON(plan EncodingPlan, predicateLabel string, targetVMAF float64) (string, error) {
	shots := make([]shotWire, 0, len(plan.Recommendations))
	for _, rec := range plan.Recommendations {
		shots = append(shots, shotWire{
			BitratekBps:   shotBitrate(rec.BitratekBps),
			CRF:           rec.CRF,
			EndFrame:      rec.Shot.EndFrame,
			PredictedVMAF: pyFloat(rec.PredictedVMAF),
			StartFrame:    rec.Shot.StartFrame,
		})
	}
	segments := plan.SegmentCommands
	if segments == nil {
		segments = [][]string{}
	}
	doc := planWire{
		ConcatCommand:   plan.ConcatCommand,
		Encoder:         plan.Encoder,
		Framerate:       pyFloat(plan.Framerate),
		Predicate:       predicateLabel,
		SegmentCommands: segments,
		Shots:           shots,
		TargetVMAF:      pyFloat(targetVMAF),
	}
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetIndent("", "  ")
	// Python's json.dumps leaves <, > and & alone; Go's default marshaller
	// escapes all three to their \u00XX form. A source path containing any
	// of them would otherwise make the two emitters disagree.
	enc.SetEscapeHTML(false)
	if err := enc.Encode(doc); err != nil {
		return "", fmt.Errorf("marshal per-shot plan: %w", err)
	}
	// json.Encoder.Encode appends a newline; RenderPlanJSON's contract is to
	// return the document without one (callers add it).
	out := strings.TrimSuffix(buf.String(), "\n")
	return ensureASCII(out), nil
}

// ensureASCII escapes every non-ASCII rune as a \uXXXX sequence, matching
// Python's json.dumps(ensure_ascii=True) default. Astral-plane runes become
// surrogate pairs, exactly as CPython emits them.
//
// The input is always well-formed JSON produced by encoding/json, so any
// non-ASCII rune can only appear inside a string literal — no quoting-state
// tracking is needed.
func ensureASCII(s string) string {
	if isASCII(s) {
		return s
	}
	var sb strings.Builder
	sb.Grow(len(s))
	for _, r := range s {
		switch {
		case r < utf8.RuneSelf:
			// #nosec G115 -- this switch arm is reached only when
			// r < utf8.RuneSelf (128), so the conversion to byte cannot
			// truncate. gosec does not narrow the type from the case guard.
			sb.WriteByte(byte(r))
		case r > 0xFFFF:
			r -= 0x10000
			fmt.Fprintf(&sb, "\\u%04x\\u%04x",
				0xD800+(r>>10), 0xDC00+(r&0x3FF))
		default:
			fmt.Fprintf(&sb, "\\u%04x", r)
		}
	}
	return sb.String()
}

// isASCII reports whether s contains only bytes below 0x80.
func isASCII(s string) bool {
	for i := range len(s) {
		if s[i] >= utf8.RuneSelf {
			return false
		}
	}
	return true
}
