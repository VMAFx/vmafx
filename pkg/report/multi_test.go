// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package report_test

import (
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/report"
)

// ---- MultiReportFromCompare ----

func TestMultiReportFromCompare_Kind(t *testing.T) {
	p := report.CompareWirePayload{
		Src:        "src.mp4",
		TargetVMAF: 85,
		Rows:       nil,
	}
	mr := report.MultiReportFromCompare(p, "results.json")
	if mr.Kind != report.KindCompare {
		t.Fatalf("want KindCompare, got %q", mr.Kind)
	}
	if mr.Src != "src.mp4" {
		t.Fatalf("want src.mp4, got %q", mr.Src)
	}
	if mr.TargetVMAF != 85 {
		t.Fatalf("want 85, got %v", mr.TargetVMAF)
	}
}

// ---- MultiReportFromLadder ----

func TestMultiReportFromLadder_Kind(t *testing.T) {
	p := report.LadderWirePayload{
		Src:         "clip.yuv",
		Encoder:     "libx264",
		TargetVMAFs: []float64{75, 85, 95},
		Resolutions: []string{"640x480", "1280x720"},
		Renditions:  []report.LadderRendition{{Width: 1280, Height: 720, BitratekBps: 2500, VMAF: 93.1, CRF: 22}},
	}
	mr := report.MultiReportFromLadder(p, "ladder.json")
	if mr.Kind != report.KindLadder {
		t.Fatalf("want KindLadder, got %q", mr.Kind)
	}
	if mr.LadderEncoder != "libx264" {
		t.Fatalf("want libx264, got %q", mr.LadderEncoder)
	}
	if len(mr.LadderRenditions) != 1 {
		t.Fatalf("want 1 rendition, got %d", len(mr.LadderRenditions))
	}
}

// ---- RenderMarkdownMulti ----

func TestRenderMarkdownMulti_Empty(t *testing.T) {
	out := report.RenderMarkdownMulti(nil)
	if !strings.Contains(out, "vmafx-tune report") {
		t.Fatalf("expected title in output; got %q", out)
	}
	if !strings.Contains(out, "0 input(s)") {
		t.Fatalf("expected zero-input annotation; got %q", out)
	}
}

func TestRenderMarkdownMulti_CompareRow(t *testing.T) {
	bitrate := 1234.5
	vmaf := 87.32
	enc := 2300.0
	rows := []report.WireRow{
		{
			Codec:          "libx264",
			EncoderVersion: "x264 r3108",
			BestCRF:        24,
			BitratekBps:    &bitrate,
			EncodeTimeMS:   &enc,
			VMAFScore:      &vmaf,
			TargetVMAF:     85,
			OK:             true,
		},
	}
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:        "src.mp4",
		TargetVMAF: 85,
		Rows:       rows,
	}, "results.json")

	out := report.RenderMarkdownMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "libx264") {
		t.Errorf("expected codec in output; got %q", out)
	}
	if !strings.Contains(out, "1234.5") {
		t.Errorf("expected bitrate in output; got %q", out)
	}
	if !strings.Contains(out, "87.32") {
		t.Errorf("expected VMAF in output; got %q", out)
	}
	if !strings.Contains(out, "| 1 |") {
		t.Errorf("expected rank 1 in output; got %q", out)
	}
}

func TestRenderMarkdownMulti_FailedRow(t *testing.T) {
	rows := []report.WireRow{
		{
			Codec: "libx265",
			OK:    false,
			Error: "no CRF achieves target",
		},
	}
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:        "src.mp4",
		TargetVMAF: 95,
		Rows:       rows,
	}, "r.json")

	out := report.RenderMarkdownMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "fail: no CRF achieves target") {
		t.Errorf("expected fail message in output; got %q", out)
	}
	if strings.Contains(out, "| 1 |") {
		t.Errorf("unexpected rank 1 for failed row; got %q", out)
	}
}

func TestRenderMarkdownMulti_LadderRenditions(t *testing.T) {
	mr := report.MultiReportFromLadder(report.LadderWirePayload{
		Src:         "clip.mp4",
		Encoder:     "libx265",
		TargetVMAFs: []float64{85},
		Renditions: []report.LadderRendition{
			{Width: 1920, Height: 1080, BitratekBps: 4200, VMAF: 94.1, CRF: 20},
			{Width: 1280, Height: 720, BitratekBps: 2100, VMAF: 88.7, CRF: 24},
		},
	}, "ladder.json")

	out := report.RenderMarkdownMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "1920x1080") {
		t.Errorf("expected 1920x1080 in output; got %q", out)
	}
	if !strings.Contains(out, "4200.0") {
		t.Errorf("expected bitrate 4200.0 in output; got %q", out)
	}
}

func TestRenderMarkdownMulti_LadderEmpty(t *testing.T) {
	mr := report.MultiReportFromLadder(report.LadderWirePayload{
		Src:        "clip.mp4",
		Encoder:    "libx264",
		Renditions: nil,
	}, "ladder.json")

	out := report.RenderMarkdownMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "No renditions selected") {
		t.Errorf("expected no-renditions message; got %q", out)
	}
}

// ---- RenderHTMLMulti ----

func TestRenderHTMLMulti_WellFormed(t *testing.T) {
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:  "src.mp4",
		Rows: nil,
	}, "r.json")
	out := report.RenderHTMLMulti([]report.MultiReport{mr})

	for _, want := range []string{"<!DOCTYPE html>", "<html", "</html>", "<table>", "</table>"} {
		if !strings.Contains(out, want) {
			t.Errorf("expected %q in HTML output", want)
		}
	}
}

func TestRenderHTMLMulti_XSSEscaping(t *testing.T) {
	evil := `<script>alert('xss')</script>`
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:  evil,
		Rows: nil,
	}, "r.json")
	out := report.RenderHTMLMulti([]report.MultiReport{mr})
	if strings.Contains(out, "<script>") {
		t.Errorf("XSS: unescaped <script> in HTML output")
	}
}

// ---- SortOrder: successful rows before failed ----

func TestRenderMarkdownMulti_SortOrder(t *testing.T) {
	b1 := 1000.0
	b2 := 500.0
	v1, v2 := 87.0, 89.0
	rows := []report.WireRow{
		{Codec: "codec-fail", OK: false, Error: "err"},
		{Codec: "codec-hi", OK: true, BestCRF: 22, BitratekBps: &b1, VMAFScore: &v1},
		{Codec: "codec-lo", OK: true, BestCRF: 26, BitratekBps: &b2, VMAFScore: &v2},
	}
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:  "src.mp4",
		Rows: rows,
	}, "r.json")

	out := report.RenderMarkdownMulti([]report.MultiReport{mr})
	idxLo := strings.Index(out, "codec-lo")
	idxHi := strings.Index(out, "codec-hi")
	idxFail := strings.Index(out, "codec-fail")

	if idxLo < 0 || idxHi < 0 || idxFail < 0 {
		t.Fatalf("one or more codec names missing from output")
	}
	// codec-lo (lower bitrate) should appear before codec-hi.
	if idxLo > idxHi {
		t.Errorf("expected codec-lo (500 kbps) before codec-hi (1000 kbps) in output")
	}
	// Both successful rows should appear before the failed row.
	if idxHi > idxFail {
		t.Errorf("expected successful rows before failed row")
	}
}
