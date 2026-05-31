// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/report/sanitize_test.go — regression coverage for the NaN-in-
// bisect_samples JSON-marshal crash. The prior EmitJSON shape declared
// BisectSamples as []bisect.Sample (float64 fields, not any), so a single
// NaN sample propagated by a corrupt vmaf XML mean would crash
// json.MarshalIndent with "json: unsupported value: NaN", aborting the
// whole compare report and breaking the Python ↔ Go parser-parity
// invariant (AGENTS.md rebase-sensitive invariant #2).

package report_test

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/report"
)

// TestEmitJSON_NaNInBisectSamplesDoesNotCrash is the canonical regression:
// a single NaN inside a bisect.Sample (for example, BitratekBps when
// ffprobe failed and the encoder propagated NaN) must not crash
// json.MarshalIndent — it must be coerced to JSON null, matching the
// recursive _nan_to_none discipline in tools/vmaf-tune/src/vmaftune/
// compare.py.
func TestEmitJSON_NaNInBisectSamplesDoesNotCrash(t *testing.T) {
	t.Parallel()

	rep := report.Report{
		Src:         "src.mp4",
		TargetVMAF:  90.0,
		ToolVersion: "test",
		WallTimeMS:  1234.0,
		Rows: []report.Row{{
			Codec:        "libx264",
			BestCRF:      23,
			BitratekBps:  1500.0,
			VMAFScore:    90.5,
			EncodeTimeMS: 1000.0,
			TargetVMAF:   90.0,
			OK:           true,
			BisectSamples: []bisect.Sample{
				{CRF: 18, BitratekBps: 4000.0, VMAFScore: 92.1, EncodeTimeMS: 1200.0},
				// Corrupt sample: NaN bitrate, NaN vmaf, +Inf encode time.
				{CRF: 24, BitratekBps: math.NaN(), VMAFScore: math.NaN(), EncodeTimeMS: math.Inf(1)},
				{CRF: 23, BitratekBps: 1500.0, VMAFScore: 90.5, EncodeTimeMS: 1000.0},
			},
		}},
	}

	out, err := report.EmitJSON(rep)
	if err != nil {
		t.Fatalf("EmitJSON returned error on NaN-in-samples: %v", err)
	}
	if strings.Contains(out, "NaN") || strings.Contains(out, "Infinity") {
		t.Errorf("EmitJSON output still contains bare NaN/Infinity tokens, breaks RFC 8259:\n%s", out)
	}

	// Round-trip parse: the output must be valid RFC 8259 JSON.
	var got map[string]any
	if err := json.Unmarshal([]byte(out), &got); err != nil {
		t.Fatalf("EmitJSON output is not valid JSON: %v\n%s", err, out)
	}

	// Walk the JSON to verify the corrupt sample's non-finite fields are
	// represented as JSON null (not omitted, not bare NaN).
	rows, _ := got["rows"].([]any)
	if len(rows) != 1 {
		t.Fatalf("expected 1 row, got %d", len(rows))
	}
	row, _ := rows[0].(map[string]any)
	samples, _ := row["bisect_samples"].([]any)
	if len(samples) != 3 {
		t.Fatalf("expected 3 samples, got %d", len(samples))
	}
	corruptSample, _ := samples[1].(map[string]any)
	if corruptSample["bitrate_kbps"] != nil {
		t.Errorf("corrupt sample bitrate_kbps: got %v, want null", corruptSample["bitrate_kbps"])
	}
	if corruptSample["vmaf_score"] != nil {
		t.Errorf("corrupt sample vmaf_score: got %v, want null", corruptSample["vmaf_score"])
	}
	if corruptSample["encode_time_ms"] != nil {
		t.Errorf("corrupt sample encode_time_ms: got %v, want null", corruptSample["encode_time_ms"])
	}
}

// TestSanitizeBisectSamples_EmptyReturnsNil ensures the helper preserves
// the omitempty contract: an empty Samples slice yields a nil any-slice,
// which then disappears from the emitted JSON entirely (no
// "bisect_samples": [] field).
func TestSanitizeBisectSamples_EmptyReturnsNil(t *testing.T) {
	t.Parallel()
	if got := report.SanitizeBisectSamples(nil); got != nil {
		t.Errorf("SanitizeBisectSamples(nil) = %v, want nil", got)
	}
	if got := report.SanitizeBisectSamples([]bisect.Sample{}); got != nil {
		t.Errorf("SanitizeBisectSamples([]) = %v, want nil", got)
	}
}

// TestSanitizeBisectSamples_FinitePreserved verifies finite floats round-
// trip unchanged through the sanitiser.
func TestSanitizeBisectSamples_FinitePreserved(t *testing.T) {
	t.Parallel()
	in := []bisect.Sample{
		{CRF: 23, BitratekBps: 1500.0, VMAFScore: 90.5, EncodeTimeMS: 1000.0},
	}
	out := report.SanitizeBisectSamples(in)
	if len(out) != 1 {
		t.Fatalf("len(out) = %d, want 1", len(out))
	}
	b, err := json.Marshal(out[0])
	if err != nil {
		t.Fatalf("marshal sanitised sample: %v", err)
	}
	for _, want := range []string{`"crf":23`, `"bitrate_kbps":1500`, `"vmaf_score":90.5`, `"encode_time_ms":1000`} {
		if !strings.Contains(string(b), want) {
			t.Errorf("output %q missing %q", string(b), want)
		}
	}
}
