// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/report/report_test.go — unit tests for the Markdown/JSON report
// renderer. Covers nanToNull's NaN/Inf coercion, Best() row selection,
// EmitJSON's schema fidelity (round-trip parses cleanly), and
// EmitMarkdown's per-row formatting branches.

package report

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/bisect"
)

// TestNanToNull covers NaN, +Inf, -Inf, and finite values.
func TestNanToNull(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		in   float64
		want any
	}{
		{"nan", math.NaN(), nil},
		{"+inf", math.Inf(1), nil},
		{"-inf", math.Inf(-1), nil},
		{"zero", 0.0, 0.0},
		{"positive", 1234.5, 1234.5},
		{"negative", -7.0, -7.0},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := nanToNull(tc.in)
			if got == nil && tc.want != nil {
				t.Errorf("nanToNull(%v) = nil, want %v", tc.in, tc.want)
			}
			if got != nil && tc.want == nil {
				t.Errorf("nanToNull(%v) = %v, want nil", tc.in, got)
			}
			if g, ok := got.(float64); ok {
				if w, _ := tc.want.(float64); g != w {
					t.Errorf("nanToNull(%v) = %v, want %v", tc.in, g, w)
				}
			}
		})
	}
}

// TestReportBest_FirstOKWins verifies Best() returns the first OK row.
func TestReportBest_FirstOKWins(t *testing.T) {
	t.Parallel()
	r := &Report{
		Rows: []Row{
			{Codec: "libx264", OK: false, Error: "boom"},
			{Codec: "libx265", OK: true, BestCRF: 22, BitratekBps: 1500.0, VMAFScore: 90.0},
			{Codec: "libaom-av1", OK: true, BestCRF: 30, BitratekBps: 900.0, VMAFScore: 88.0},
		},
	}
	best := r.Best()
	if best == nil {
		t.Fatal("Best() returned nil")
	}
	if best.Codec != "libx265" {
		t.Errorf("Best().Codec = %q, want %q", best.Codec, "libx265")
	}
}

// TestReportBest_AllFailedReturnsNil exercises the all-fail branch.
func TestReportBest_AllFailedReturnsNil(t *testing.T) {
	t.Parallel()
	r := &Report{
		Rows: []Row{
			{Codec: "libx264", OK: false, Error: "decoder error"},
			{Codec: "libx265", OK: false, Error: "encoder error"},
		},
	}
	if r.Best() != nil {
		t.Error("Best() expected nil for all-failed report")
	}
}

// TestReportBest_EmptyRowsReturnsNil covers the empty-rows branch.
func TestReportBest_EmptyRowsReturnsNil(t *testing.T) {
	t.Parallel()
	r := &Report{Rows: nil}
	if r.Best() != nil {
		t.Error("Best() expected nil for empty rows")
	}
}

// TestEmitJSON_RoundTrip ensures the rendered JSON parses back to the
// expected shape with proper null-coercion of NaN.
func TestEmitJSON_RoundTrip(t *testing.T) {
	t.Parallel()
	rep := Report{
		Src:         "/tmp/src.mp4",
		TargetVMAF:  90.0,
		ToolVersion: "test-vN",
		WallTimeMS:  1234.5,
		Rows: []Row{
			{
				Codec:        "libx264",
				BestCRF:      20,
				BitratekBps:  1000.0,
				EncodeTimeMS: 50.0,
				VMAFScore:    91.5,
				TargetVMAF:   90.0,
				OK:           true,
			},
			{
				Codec:        "libx265",
				BestCRF:      -1,
				BitratekBps:  math.NaN(),
				EncodeTimeMS: math.NaN(),
				VMAFScore:    math.NaN(),
				OK:           false,
				Error:        "failed",
			},
		},
	}
	out, err := EmitJSON(rep)
	if err != nil {
		t.Fatalf("EmitJSON: %v", err)
	}
	if !strings.HasSuffix(out, "\n") {
		t.Error("EmitJSON output must end with newline")
	}

	var decoded struct {
		Src         string  `json:"src"`
		TargetVMAF  float64 `json:"target_vmaf"`
		ToolVersion string  `json:"tool_version"`
		Rows        []struct {
			Codec        string `json:"codec"`
			BitratekBps  any    `json:"bitrate_kbps"`
			EncodeTimeMS any    `json:"encode_time_ms"`
			VMAFScore    any    `json:"vmaf_score"`
			OK           bool   `json:"ok"`
			Error        string `json:"error"`
		} `json:"rows"`
	}
	if err := json.Unmarshal([]byte(out), &decoded); err != nil {
		t.Fatalf("decode: %v\nraw: %s", err, out)
	}
	if decoded.Src != "/tmp/src.mp4" {
		t.Errorf("Src = %q, want %q", decoded.Src, "/tmp/src.mp4")
	}
	if decoded.ToolVersion != "test-vN" {
		t.Errorf("ToolVersion = %q, want %q", decoded.ToolVersion, "test-vN")
	}
	if len(decoded.Rows) != 2 {
		t.Fatalf("rows = %d, want 2", len(decoded.Rows))
	}
	// Row 1 finite floats → preserved as numbers.
	if _, ok := decoded.Rows[0].BitratekBps.(float64); !ok {
		t.Errorf("row 0 BitratekBps = %T %v, want float64", decoded.Rows[0].BitratekBps, decoded.Rows[0].BitratekBps)
	}
	// Row 2 NaN/NaN/NaN → null in JSON.
	if decoded.Rows[1].BitratekBps != nil {
		t.Errorf("row 1 BitratekBps = %v, want nil", decoded.Rows[1].BitratekBps)
	}
	if decoded.Rows[1].VMAFScore != nil {
		t.Errorf("row 1 VMAFScore = %v, want nil", decoded.Rows[1].VMAFScore)
	}
}

// TestEmitJSON_EmptyRows verifies empty-rows reports serialise cleanly.
func TestEmitJSON_EmptyRows(t *testing.T) {
	t.Parallel()
	rep := Report{Src: "/x", TargetVMAF: 80.0, ToolVersion: "v1"}
	out, err := EmitJSON(rep)
	if err != nil {
		t.Fatalf("EmitJSON: %v", err)
	}
	var decoded map[string]any
	if err := json.Unmarshal([]byte(out), &decoded); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if decoded["src"] != "/x" {
		t.Errorf("src = %v, want /x", decoded["src"])
	}
}

// TestEmitJSON_PreservesBisectSamples verifies the additive samples field
// is included when present (ADR-0530).
func TestEmitJSON_PreservesBisectSamples(t *testing.T) {
	t.Parallel()
	rep := Report{
		Rows: []Row{{
			Codec: "libx264",
			OK:    true,
			BisectSamples: []bisect.Sample{
				{CRF: 22, BitratekBps: 1100.0, VMAFScore: 90.0, EncodeTimeMS: 20.0},
				{CRF: 26, BitratekBps: 800.0, VMAFScore: 86.0, EncodeTimeMS: 18.0},
			},
		}},
	}
	out, err := EmitJSON(rep)
	if err != nil {
		t.Fatalf("EmitJSON: %v", err)
	}
	if !strings.Contains(out, `"bisect_samples"`) {
		t.Errorf("expected bisect_samples key in JSON, got: %s", out)
	}
	if !strings.Contains(out, `"crf": 22`) {
		t.Errorf("expected crf:22 in JSON, got: %s", out)
	}
}

// TestEmitMarkdown_OKRow renders a passing row and verifies the table
// columns + summary line are present.
func TestEmitMarkdown_OKRow(t *testing.T) {
	t.Parallel()
	rep := Report{
		Src:        "/tmp/src.mp4",
		TargetVMAF: 90.0,
		Rows: []Row{{
			Codec:          "libx264",
			EncoderVersion: "core 164",
			BestCRF:        20,
			BitratekBps:    1500.0,
			EncodeTimeMS:   42.0,
			VMAFScore:      91.5,
			OK:             true,
		}},
	}
	md := EmitMarkdown(rep)
	mustContain := []string{
		"# Codec comparison",
		"libx264",
		"core 164",
		"| 1 |",      // rank
		"**Smallest", // best-row footer
	}
	for _, sub := range mustContain {
		if !strings.Contains(md, sub) {
			t.Errorf("EmitMarkdown missing %q\noutput:\n%s", sub, md)
		}
	}
}

// TestEmitMarkdown_FailRow exercises the failed-row branch (rankCell "—",
// status "fail: <error>").
func TestEmitMarkdown_FailRow(t *testing.T) {
	t.Parallel()
	rep := Report{
		Rows: []Row{{
			Codec:       "libx264",
			OK:          false,
			Error:       "encoder crashed",
			BestCRF:     -1,
			BitratekBps: math.NaN(),
			VMAFScore:   math.NaN(),
		}},
	}
	md := EmitMarkdown(rep)
	if !strings.Contains(md, "fail: encoder crashed") {
		t.Errorf("failure status not rendered: %s", md)
	}
	// Best() returns nil → footer should be the no-codec message.
	if !strings.Contains(md, "No codec succeeded") {
		t.Errorf("expected no-codec-succeeded footer: %s", md)
	}
	// NaN bitrate / vmaf should appear as em-dash placeholders.
	if !strings.Contains(md, "—") {
		t.Errorf("expected em-dash placeholder for NaN values: %s", md)
	}
}

// TestEmitMarkdown_NoEncoderVersion exercises the empty-version branch
// which falls back to an em-dash.
func TestEmitMarkdown_NoEncoderVersion(t *testing.T) {
	t.Parallel()
	rep := Report{
		Rows: []Row{{
			Codec:          "libx264",
			EncoderVersion: "",
			BestCRF:        22,
			BitratekBps:    1000.0,
			EncodeTimeMS:   10.0,
			VMAFScore:      88.0,
			OK:             true,
		}},
	}
	md := EmitMarkdown(rep)
	if !strings.Contains(md, "libx264") {
		t.Errorf("codec missing: %s", md)
	}
}
