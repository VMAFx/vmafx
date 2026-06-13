// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/unit_internal_test.go — in-package unit tests covering
// the pure-Go helpers in compare.go, ladder.go, and root.go that the existing
// _test.go (integration) suite does not exercise because it shells out to
// the built binary.
//
// Coverage target: 0% → ~70% for cmd/vmafx-tune/cmd.

package cmd

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/report"
)

// ---------------------------------------------------------------------------
// parseResolution
// ---------------------------------------------------------------------------

func TestParseResolution_HappyPath(t *testing.T) {
	t.Parallel()
	cases := []struct {
		in   string
		w, h int
	}{
		{"1280x720", 1280, 720},
		{"1920X1080", 1920, 1080},
		{" 640x480 ", 640, 480},
		{"320x240", 320, 240},
	}
	for _, tc := range cases {
		w, h, err := parseResolution(tc.in)
		if err != nil {
			t.Errorf("parseResolution(%q) error: %v", tc.in, err)
			continue
		}
		if w != tc.w || h != tc.h {
			t.Errorf("parseResolution(%q) = (%d, %d), want (%d, %d)",
				tc.in, w, h, tc.w, tc.h)
		}
	}
}

func TestParseResolution_BadInput(t *testing.T) {
	t.Parallel()
	bad := []string{"", "1280", "1280x", "x720", "abcxdef", "1280x720x1", "-1x1080", "1280x-1", "0x720", "1280x0"}
	for _, in := range bad {
		_, _, err := parseResolution(in)
		if err == nil {
			t.Errorf("parseResolution(%q) expected error, got nil", in)
		}
	}
}

// ---------------------------------------------------------------------------
// failRow / rowFromBisect
// ---------------------------------------------------------------------------

func TestFailRow_FieldsSet(t *testing.T) {
	t.Parallel()
	row := failRow("libx264", 90.0, "ffmpeg", "boom")
	if row.Codec != "libx264" {
		t.Errorf("Codec: got %q", row.Codec)
	}
	if row.BestCRF != -1 {
		t.Errorf("BestCRF: got %d, want -1", row.BestCRF)
	}
	if row.OK {
		t.Error("OK should be false")
	}
	if row.Error != "boom" {
		t.Errorf("Error: got %q", row.Error)
	}
	if !math.IsNaN(row.BitratekBps) || !math.IsNaN(row.VMAFScore) || !math.IsNaN(row.EncodeTimeMS) {
		t.Errorf("expected NaN sentinels, got bitrate=%v vmaf=%v enc=%v",
			row.BitratekBps, row.VMAFScore, row.EncodeTimeMS)
	}
	if row.TargetVMAF != 90.0 {
		t.Errorf("TargetVMAF: got %v", row.TargetVMAF)
	}
}

func TestRowFromBisect_OKResult(t *testing.T) {
	t.Parallel()
	res := bisect.Result{
		BestCRF:          23,
		BestBitratekBps:  1500.5,
		BestEncodeTimeMS: 1200.0,
		BestVMAFScore:    90.5,
		EncoderVersion:   "core 164",
		Samples:          []bisect.Sample{{CRF: 23, VMAFScore: 90.5, BitratekBps: 1500.5}},
	}
	row := rowFromBisect("libx264", 90.0, "ffmpeg", res)
	if !row.OK {
		t.Error("OK should be true")
	}
	if row.BestCRF != 23 {
		t.Errorf("BestCRF: got %d, want 23", row.BestCRF)
	}
	if row.BitratekBps != 1500.5 {
		t.Errorf("BitratekBps: got %v, want 1500.5", row.BitratekBps)
	}
	if row.EncoderVersion != "core 164" {
		t.Errorf("EncoderVersion: got %q", row.EncoderVersion)
	}
	if len(row.BisectSamples) != 1 {
		t.Errorf("BisectSamples len: got %d, want 1", len(row.BisectSamples))
	}
}

func TestRowFromBisect_NoCRFFound(t *testing.T) {
	t.Parallel()
	res := bisect.Result{BestCRF: -1}
	row := rowFromBisect("libx265", 95.0, "ffmpeg", res)
	if row.OK {
		t.Error("OK should be false")
	}
	if row.BestCRF != -1 {
		t.Errorf("BestCRF: got %d, want -1", row.BestCRF)
	}
	if !strings.Contains(row.Error, "no CRF") {
		t.Errorf("Error should mention no CRF, got %q", row.Error)
	}
	if !math.IsNaN(row.BitratekBps) {
		t.Errorf("BitratekBps should be NaN, got %v", row.BitratekBps)
	}
}

// ---------------------------------------------------------------------------
// sortRows
// ---------------------------------------------------------------------------

func TestSortRows_OKBeforeFailAscByBitrate(t *testing.T) {
	t.Parallel()
	rows := []report.Row{
		{Codec: "libx265", BitratekBps: 1200.0, OK: true},
		{Codec: "libx264", BitratekBps: 1500.0, OK: true},
		{Codec: "libsvtav1", BitratekBps: 900.0, OK: true},
		{Codec: "libaom-av1", OK: false, Error: "fail"},
		{Codec: "h264_nvenc", OK: false, Error: "fail"},
	}
	sortRows(rows)
	// First three: OK rows, ascending by bitrate.
	wantOrder := []string{"libsvtav1", "libx265", "libx264", "h264_nvenc", "libaom-av1"}
	for i, want := range wantOrder {
		if rows[i].Codec != want {
			t.Errorf("position %d: got %q, want %q (order: %v)",
				i, rows[i].Codec, want, codecs(rows))
		}
	}
}

func TestSortRows_OKEqualBitrateByCodec(t *testing.T) {
	t.Parallel()
	rows := []report.Row{
		{Codec: "libx265", BitratekBps: 1500.0, OK: true},
		{Codec: "libx264", BitratekBps: 1500.0, OK: true},
	}
	sortRows(rows)
	if rows[0].Codec != "libx264" {
		t.Errorf("equal bitrate should tiebreak alphabetically, got %v", codecs(rows))
	}
}

func codecs(rows []report.Row) []string {
	out := make([]string, len(rows))
	for i, r := range rows {
		out[i] = r.Codec
	}
	return out
}

// ---------------------------------------------------------------------------
// emitSweepJSON
// ---------------------------------------------------------------------------

func TestEmitSweepJSON_Schema(t *testing.T) {
	t.Parallel()
	results := []pairResult{
		{order: 0, row: report.Row{
			Codec: "libx264", BestCRF: 23, BitratekBps: 1500.0,
			VMAFScore: 90.5, EncodeTimeMS: 1200.0, TargetVMAF: 90.0, OK: true,
		}},
		{order: 1, row: report.Row{
			Codec: "libx265", BestCRF: -1, BitratekBps: math.NaN(),
			VMAFScore: math.NaN(), EncodeTimeMS: math.NaN(),
			TargetVMAF: 90.0, OK: false, Error: "fail",
		}},
	}
	flags := &compareFlags{reference: "src.mp4", targets: []float64{90.0}}

	out, err := emitSweepJSON(results, flags, 1234.5)
	if err != nil {
		t.Fatalf("emitSweepJSON: %v", err)
	}

	var got struct {
		SchemaVersion int       `json:"schema_version"`
		Src           string    `json:"src"`
		TargetVMAFs   []float64 `json:"target_vmafs"`
		WallTimeMS    float64   `json:"wall_time_ms"`
		Rows          []struct {
			Codec       string  `json:"codec"`
			BestCRF     int     `json:"best_crf"`
			BitratekBps any     `json:"bitrate_kbps"`
			OK          bool    `json:"ok"`
			TargetVMAF  float64 `json:"target_vmaf"`
		} `json:"rows"`
	}
	if err := json.Unmarshal([]byte(out), &got); err != nil {
		t.Fatalf("Unmarshal output: %v\noutput=%s", err, out)
	}
	if got.SchemaVersion != 2 {
		t.Errorf("SchemaVersion: got %d, want 2", got.SchemaVersion)
	}
	if got.Src != "src.mp4" {
		t.Errorf("Src: got %q", got.Src)
	}
	if got.WallTimeMS != 1234.5 {
		t.Errorf("WallTimeMS: got %v", got.WallTimeMS)
	}
	if len(got.Rows) != 2 {
		t.Fatalf("Rows len: got %d, want 2", len(got.Rows))
	}
	// Fail row's bitrate_kbps must be JSON null (nan2null).
	if got.Rows[1].BitratekBps != nil {
		t.Errorf("Rows[1].BitratekBps: got %v, want null", got.Rows[1].BitratekBps)
	}
	// OK row's bitrate_kbps must be a float.
	if _, ok := got.Rows[0].BitratekBps.(float64); !ok {
		t.Errorf("Rows[0].BitratekBps should be float64, got %T", got.Rows[0].BitratekBps)
	}
}

// ---------------------------------------------------------------------------
// emitSweepMarkdown
// ---------------------------------------------------------------------------

func TestEmitSweepMarkdown_Smoke(t *testing.T) {
	t.Parallel()
	results := []pairResult{
		{order: 0, row: report.Row{
			Codec: "libx264", BestCRF: 23, BitratekBps: 1500.0,
			VMAFScore: 90.5, EncodeTimeMS: 1200.0, TargetVMAF: 90.0,
			OK: true, EncoderVersion: "core 164",
		}},
		{order: 1, row: report.Row{
			Codec: "libx265", BestCRF: -1, BitratekBps: math.NaN(),
			VMAFScore: math.NaN(), EncodeTimeMS: math.NaN(),
			TargetVMAF: 90.0, OK: false, Error: "encoder failed",
		}},
	}
	flags := &compareFlags{reference: "src.mp4", targets: []float64{90.0, 95.0}}
	md := emitSweepMarkdown(results, flags, 999.0)

	want := []string{
		"# Codec rate-quality sweep",
		"src.mp4",
		"libx264",
		"libx265",
		"encoder failed",
		"core 164",
		"1500.0",
		"90.50",
		"ok",
		"fail",
		"| Codec |",
	}
	for _, s := range want {
		if !strings.Contains(md, s) {
			t.Errorf("expected Markdown to contain %q, full text:\n%s", s, md)
		}
	}
}

func TestEmitSweepMarkdown_FailRowSentinels(t *testing.T) {
	t.Parallel()
	// fail row with empty encoder version → renders the em-dash placeholder.
	results := []pairResult{{order: 0, row: report.Row{
		Codec: "libaom-av1", BestCRF: -1, BitratekBps: math.NaN(),
		VMAFScore: math.NaN(), EncodeTimeMS: math.NaN(),
		TargetVMAF: 95.0, OK: false, Error: "missing",
	}}}
	flags := &compareFlags{reference: "x.mp4", targets: []float64{95.0}}
	md := emitSweepMarkdown(results, flags, 0.0)
	// The em-dash "—" should appear (3 dashes: enc version, bitrate, vmaf).
	dashCount := strings.Count(md, "—")
	if dashCount < 3 {
		t.Errorf("expected ≥3 em-dash sentinels for fail row, got %d in:\n%s", dashCount, md)
	}
}

// ---------------------------------------------------------------------------
// writeOutput
// ---------------------------------------------------------------------------

func TestWriteOutput_ToFile(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	path := filepath.Join(dir, "nested", "out.json")
	if err := writeOutput(path, "hello world"); err != nil {
		t.Fatalf("writeOutput: %v", err)
	}
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if string(body) != "hello world" {
		t.Errorf("contents: got %q, want %q", body, "hello world")
	}
}

func TestWriteOutput_Stdout(t *testing.T) {
	t.Parallel()
	// Empty path → fmt.Print to stdout; we just verify no error.
	if err := writeOutput("", ""); err != nil {
		t.Errorf("writeOutput('', ''): %v", err)
	}
}

// ---------------------------------------------------------------------------
// emitLadderJSON / emitLadderMarkdown
// ---------------------------------------------------------------------------

func TestEmitLadderJSON_Schema(t *testing.T) {
	t.Parallel()
	// Build a minimal LadderResult by importing the ladder package types.
	// We construct it directly via type literal rather than calling Build.
	flags := &ladderFlags{
		reference:   "src.mp4",
		resolutions: []string{"640x480", "1920x1080"},
		targets:     []float64{85.0, 95.0},
	}
	res := makeLadderResult()
	out, err := emitLadderJSON(res, flags, 1234.5)
	if err != nil {
		t.Fatalf("emitLadderJSON: %v", err)
	}
	var got ladderWirePayload
	if err := json.Unmarshal([]byte(out), &got); err != nil {
		t.Fatalf("Unmarshal: %v\nout=%s", err, out)
	}
	if got.SchemaVersion != 1 {
		t.Errorf("SchemaVersion: got %d, want 1", got.SchemaVersion)
	}
	if got.WallTimeMS != 1234.5 {
		t.Errorf("WallTimeMS: got %v, want 1234.5", got.WallTimeMS)
	}
	if len(got.Resolutions) != 2 {
		t.Errorf("Resolutions len: got %d, want 2", len(got.Resolutions))
	}
	if len(got.Cloud) == 0 {
		t.Error("expected non-empty cloud")
	}
}

func TestEmitLadderJSON_SanitisesNaNInCloud(t *testing.T) {
	t.Parallel()
	flags := &ladderFlags{
		reference:   "src.mp4",
		resolutions: []string{"640x480"},
		targets:     []float64{85.0},
	}
	res := makeLadderResultWithNaN()
	out, err := emitLadderJSON(res, flags, 100.0)
	if err != nil {
		t.Fatalf("emitLadderJSON: %v", err)
	}
	// NaN bitrate is coerced to 0 (not null, per the ladder JSON convention).
	// Output must be valid JSON with no "NaN" tokens.
	if strings.Contains(out, "NaN") || strings.Contains(out, "Infinity") {
		t.Errorf("expected NaN/Inf to be sanitised, got:\n%s", out)
	}
	var got ladderWirePayload
	if err := json.Unmarshal([]byte(out), &got); err != nil {
		t.Fatalf("Unmarshal: %v", err)
	}
	for _, p := range got.Cloud {
		if math.IsNaN(p.BitratekBps) {
			t.Error("cloud point still has NaN bitrate after sanitisation")
		}
	}
}

func TestEmitLadderMarkdown_Smoke(t *testing.T) {
	t.Parallel()
	flags := &ladderFlags{
		reference:   "src.mp4",
		resolutions: []string{"640x480", "1920x1080"},
		targets:     []float64{85.0, 95.0},
	}
	res := makeLadderResult()
	md := emitLadderMarkdown(res, flags, 999.0)
	for _, s := range []string{
		"# Per-title ABR ladder", "src.mp4", "## Selected renditions",
		"## Convex hull (Pareto frontier)", "640x480", "85", "95",
	} {
		if !strings.Contains(md, s) {
			t.Errorf("expected Markdown to contain %q, full:\n%s", s, md)
		}
	}
}

func TestEmitLadderMarkdown_EmptyHullAndRenditions(t *testing.T) {
	t.Parallel()
	flags := &ladderFlags{
		reference:   "x.mp4",
		resolutions: []string{"320x240"},
		targets:     []float64{85.0},
	}
	res := makeEmptyLadderResult()
	md := emitLadderMarkdown(res, flags, 0.0)
	// Empty-state messages must appear.
	if !strings.Contains(md, "No renditions selected") {
		t.Errorf("expected empty-renditions message, got:\n%s", md)
	}
	if !strings.Contains(md, "Hull is empty") {
		t.Errorf("expected empty-hull message, got:\n%s", md)
	}
}

// ---------------------------------------------------------------------------
// stubSubcommand / Execute(version)
// ---------------------------------------------------------------------------

func TestStubSubcommand_ReturnsError(t *testing.T) {
	t.Parallel()
	cmd := stubSubcommand("fast", "Fast NR-proxy accelerated tune")
	if cmd.Use != "fast" {
		t.Errorf("Use: got %q", cmd.Use)
	}
	if !strings.Contains(cmd.Short, "not yet ported") {
		t.Errorf("Short should mention not ported: %q", cmd.Short)
	}
	if !strings.Contains(cmd.Long, "vmaf-tune") {
		t.Errorf("Long should reference vmaf-tune: %q", cmd.Long)
	}
	// Invoke RunE: must return non-nil error.
	err := cmd.RunE(cmd, nil)
	if err == nil {
		t.Fatal("RunE: expected error for stub subcommand")
	}
	if !strings.Contains(err.Error(), "not yet ported") {
		t.Errorf("error should mention not ported, got: %v", err)
	}
}

func TestNewCompareCmd_HasRequiredFlags(t *testing.T) {
	t.Parallel()
	cmd := newCompareCmd()
	if cmd.Use != "compare" {
		t.Errorf("Use: got %q", cmd.Use)
	}
	for _, flag := range []string{"reference", "codecs", "targets", "output", "format", "ffmpeg", "vmaf", "crf-lo", "crf-hi", "max-iter"} {
		if cmd.Flag(flag) == nil {
			t.Errorf("flag %q missing from compare command", flag)
		}
	}
}

func TestNewLadderCmd_HasRequiredFlags(t *testing.T) {
	t.Parallel()
	cmd := newLadderCmd()
	if cmd.Use != "ladder" {
		t.Errorf("Use: got %q", cmd.Use)
	}
	for _, flag := range []string{"reference", "codec", "resolutions", "targets", "output", "format", "ffmpeg", "vmaf", "crf-lo", "crf-hi", "max-iter", "max-rungs", "min-bitrate-gap"} {
		if cmd.Flag(flag) == nil {
			t.Errorf("flag %q missing from ladder command", flag)
		}
	}
}

// ---------------------------------------------------------------------------
// runCompare / runLadder validation paths
// ---------------------------------------------------------------------------

func TestRunCompare_ValidationErrors(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name  string
		flags compareFlags
		want  string
	}{
		{"empty-ref", compareFlags{}, "--reference is required"},
		{"missing-file", compareFlags{reference: "/no/such/file.mp4"}, "reference file"},
		{"no-codecs", compareFlags{reference: existingFile(t), codecs: nil, targets: []float64{85}}, "--codecs"},
		{"no-targets", compareFlags{reference: existingFile(t), codecs: []string{"libx264"}, targets: nil}, "--targets"},
		{"target-out-of-range", compareFlags{reference: existingFile(t), codecs: []string{"libx264"}, targets: []float64{0}}, "out of range"},
		{"target-too-high", compareFlags{reference: existingFile(t), codecs: []string{"libx264"}, targets: []float64{101}}, "out of range"},
		{"bad-format", compareFlags{reference: existingFile(t), codecs: []string{"libx264"}, targets: []float64{85}, format: "yaml"}, "unknown --format"},
		{"unknown-codec", compareFlags{reference: existingFile(t), codecs: []string{"libxyz"}, targets: []float64{85}, format: "json"}, "encoder"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := runCompare(&tc.flags)
			if err == nil {
				t.Fatalf("expected error containing %q, got nil", tc.want)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error = %v, want substring %q", err, tc.want)
			}
		})
	}
}

func TestRunLadder_ValidationErrors(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name  string
		flags ladderFlags
		want  string
	}{
		{"empty-ref", ladderFlags{}, "--reference is required"},
		{"missing-file", ladderFlags{reference: "/no/such/file.mp4"}, "reference file"},
		{"no-targets", ladderFlags{reference: existingFile(t), targets: nil}, "--targets"},
		{"target-out-of-range", ladderFlags{reference: existingFile(t), targets: []float64{0}}, "out of range"},
		{"bad-format", ladderFlags{reference: existingFile(t), targets: []float64{85}, format: "yaml", resolutions: []string{"640x480"}, codec: "libx264"}, "unknown --format"},
		{"no-resolutions", ladderFlags{reference: existingFile(t), targets: []float64{85}, resolutions: nil, codec: "libx264", format: "json"}, "--resolutions"},
		{"bad-resolution", ladderFlags{reference: existingFile(t), targets: []float64{85}, resolutions: []string{"badres"}, codec: "libx264", format: "json"}, "WxH"},
		{"unknown-codec", ladderFlags{reference: existingFile(t), targets: []float64{85}, resolutions: []string{"640x480"}, codec: "libxyz", format: "json"}, "encoder"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := runLadder(&tc.flags)
			if err == nil {
				t.Fatalf("expected error containing %q, got nil", tc.want)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error = %v, want substring %q", err, tc.want)
			}
		})
	}
}

// existingFile returns a path to an empty file that os.Stat will resolve.
func existingFile(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	p := filepath.Join(dir, "ref.mp4")
	if err := os.WriteFile(p, []byte{}, 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	return p
}
