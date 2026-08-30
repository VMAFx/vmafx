// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris

package report_test

import (
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/report"
)

// TestRenderHTMLMulti_CompareRows exercises renderCompareHTML (the internal
// path for KindCompare reports) which was previously at 30.8% coverage.
func TestRenderHTMLMulti_CompareRows(t *testing.T) {
	t.Parallel()
	bitrate := 1500.0
	encTime := 3200.0
	vmafScore := 92.45
	rows := []report.WireRow{
		{
			Codec:          "libx264",
			EncoderVersion: "x264 r3108",
			BestCRF:        22,
			BitratekBps:    &bitrate,
			EncodeTimeMS:   &encTime,
			VMAFScore:      &vmafScore,
			TargetVMAF:     90,
			OK:             true,
		},
		{
			Codec: "libx265",
			OK:    false,
			Error: "CRF sweep failed",
		},
	}
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:        "clip.mp4",
		TargetVMAF: 90,
		Rows:       rows,
	}, "results.json")

	out := report.RenderHTMLMulti([]report.MultiReport{mr})

	// Basic HTML structure.
	for _, want := range []string{"<!DOCTYPE html>", "<html", "</html>", "<table>", "</table>"} {
		if !strings.Contains(out, want) {
			t.Errorf("expected %q in HTML output", want)
		}
	}
	// Codec names must appear.
	if !strings.Contains(out, "libx264") {
		t.Errorf("libx264 missing from HTML output")
	}
	if !strings.Contains(out, "libx265") {
		t.Errorf("libx265 missing from HTML output")
	}
	// Rank should appear for the successful row.
	if !strings.Contains(out, ">1<") {
		t.Errorf("rank 1 missing from HTML output")
	}
	// The failed row should display the em-dash rank placeholder.
	if !strings.Contains(out, "&mdash;") {
		t.Errorf("em-dash rank placeholder missing from HTML output for failed row")
	}
	// VMAF value.
	if !strings.Contains(out, "92.45") {
		t.Errorf("VMAF score 92.45 missing from HTML output")
	}
}

// TestRenderHTMLMulti_LadderRows exercises renderLadderHTML — the KindLadder
// path of RenderHTMLMulti.
func TestRenderHTMLMulti_LadderRows(t *testing.T) {
	t.Parallel()
	mr := report.MultiReportFromLadder(report.LadderWirePayload{
		Src:     "clip.mp4",
		Encoder: "libx265",
		Renditions: []report.LadderRendition{
			{Width: 1920, Height: 1080, BitratekBps: 4500, VMAF: 95.3, CRF: 19},
			{Width: 1280, Height: 720, BitratekBps: 2100, VMAF: 88.1, CRF: 24},
		},
	}, "ladder.json")

	out := report.RenderHTMLMulti([]report.MultiReport{mr})

	for _, want := range []string{"<!DOCTYPE html>", "1920", "1080", "4500.0", "95.30", "2100.0"} {
		if !strings.Contains(out, want) {
			t.Errorf("expected %q in HTML output; got (truncated): %.200s", want, out)
		}
	}
}

// TestRenderHTMLMulti_LadderEmpty exercises the "no renditions" branch.
func TestRenderHTMLMulti_LadderEmpty(t *testing.T) {
	t.Parallel()
	mr := report.MultiReportFromLadder(report.LadderWirePayload{
		Src:        "clip.mp4",
		Encoder:    "libx264",
		Renditions: nil,
	}, "empty.json")

	out := report.RenderHTMLMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "No renditions selected") {
		t.Errorf("expected 'No renditions selected' in HTML output")
	}
}

// TestRenderHTMLMulti_CompareNilBitrate exercises the nil-pointer path in
// float64PtrOr: when BitratekBps / EncodeTimeMS / VMAFScore are nil the
// helper must return the fallback (math.MaxFloat64 for sort, "&mdash;" for display).
func TestRenderHTMLMulti_CompareNilBitrate(t *testing.T) {
	t.Parallel()
	rows := []report.WireRow{
		{
			Codec:  "libvpx-vp9",
			BestCRF: -1, // negative CRF → em-dash in table
			OK:     true,
			// BitratekBps, EncodeTimeMS, VMAFScore are all nil.
		},
	}
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:  "clip.webm",
		Rows: rows,
	}, "r.json")

	out := report.RenderHTMLMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "libvpx-vp9") {
		t.Errorf("codec name missing from HTML output")
	}
}

// TestRenderHTMLMulti_CompareNoEncoderVersion exercises the "&mdash;" fallback
// for an empty EncoderVersion field in the HTML table.
func TestRenderHTMLMulti_CompareNoEncoderVersion(t *testing.T) {
	t.Parallel()
	bitrate := 800.0
	vmaf := 81.5
	rows := []report.WireRow{
		{
			Codec:          "libaom-av1",
			EncoderVersion: "", // explicitly empty — falls back to &mdash; in HTML
			BestCRF:        30,
			BitratekBps:    &bitrate,
			VMAFScore:      &vmaf,
			OK:             true,
		},
	}
	mr := report.MultiReportFromCompare(report.CompareWirePayload{
		Src:  "clip.mp4",
		Rows: rows,
	}, "r.json")

	out := report.RenderHTMLMulti([]report.MultiReport{mr})
	if !strings.Contains(out, "libaom-av1") {
		t.Errorf("codec name missing from HTML output")
	}
}

// TestRenderHTMLMulti_MultipleReports ensures the outer loop in RenderHTMLMulti
// iterates over all reports (not just the first).
func TestRenderHTMLMulti_MultipleReports(t *testing.T) {
	t.Parallel()
	mr1 := report.MultiReportFromCompare(report.CompareWirePayload{Src: "clip1.mp4"}, "r1.json")
	mr2 := report.MultiReportFromLadder(report.LadderWirePayload{Src: "clip2.mp4", Encoder: "libx264"}, "r2.json")

	out := report.RenderHTMLMulti([]report.MultiReport{mr1, mr2})
	if !strings.Contains(out, "clip1.mp4") {
		t.Errorf("first report src missing from HTML output")
	}
	if !strings.Contains(out, "clip2.mp4") {
		t.Errorf("second report src missing from HTML output")
	}
}
