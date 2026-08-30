// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package executor

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/tune/auto"
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, &slog.HandlerOptions{Level: slog.LevelError}))
}

// ---------------------------------------------------------------------------
// Argv construction.
// ---------------------------------------------------------------------------

// TestBuildFFmpegCommand pins the argv the Python build_ffmpeg_command emits,
// including the load-bearing detail that -ss / -t go *before* -i: output-side
// seeking would still decode the whole source and defeat the sample-clip
// speedup entirely.
func TestBuildFFmpegCommand(t *testing.T) {
	t.Parallel()

	base := EncodeRequest{
		Source: "src.mkv", Width: 1920, Height: 1080, PixFmt: "yuv420p",
		Framerate: 25.0, Encoder: "libx264", Preset: "medium", CRF: 23,
		Output: "out.mkv", SourceIsContainer: true,
	}
	prefix := []string{"ffmpeg", "-y", "-hide_banner", "-loglevel", "info"}

	tests := []struct {
		name   string
		mutate func(*EncodeRequest)
		want   []string
	}{
		{
			name:   "container source",
			mutate: func(*EncodeRequest) {},
			want: append(append([]string{}, prefix...),
				"-i", "src.mkv", "-c:v", "libx264", "-preset", "medium", "-crf", "23", "out.mkv"),
		},
		{
			name:   "raw yuv source spells out the format",
			mutate: func(r *EncodeRequest) { r.SourceIsContainer = false },
			want: append(append([]string{}, prefix...),
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "25.0",
				"-i", "src.mkv", "-c:v", "libx264", "-preset", "medium", "-crf", "23", "out.mkv"),
		},
		{
			name: "sample clip seeks on the input side",
			mutate: func(r *EncodeRequest) {
				r.SampleClipSeconds = 10.0
				r.SampleClipStartS = 4.5
			},
			want: append(append([]string{}, prefix...),
				"-ss", "4.5", "-t", "10.0",
				"-i", "src.mkv", "-c:v", "libx264", "-preset", "medium", "-crf", "23", "out.mkv"),
		},
		{
			name:   "duration bounds the encode when sample-clip is off",
			mutate: func(r *EncodeRequest) { r.DurationS = 10.0 },
			want: append(append([]string{}, prefix...),
				"-t", "10.0",
				"-i", "src.mkv", "-c:v", "libx264", "-preset", "medium", "-crf", "23", "out.mkv"),
		},
		{
			name: "sample clip wins over duration",
			mutate: func(r *EncodeRequest) {
				r.SampleClipSeconds = 3.0
				r.DurationS = 600.0
			},
			want: append(append([]string{}, prefix...),
				"-ss", "0.0", "-t", "3.0",
				"-i", "src.mkv", "-c:v", "libx264", "-preset", "medium", "-crf", "23", "out.mkv"),
		},
		{
			name:   "nvenc gets its codec-correct knobs",
			mutate: func(r *EncodeRequest) { r.Encoder = "h264_nvenc"; r.CRF = 26 },
			want: append(append([]string{}, prefix...),
				"-i", "src.mkv", "-c:v", "h264_nvenc", "-preset", "p4", "-cq", "26", "out.mkv"),
		},
		{
			name:   "libvpx appends its extra params",
			mutate: func(r *EncodeRequest) { r.Encoder = "libvpx-vp9"; r.CRF = 32 },
			want: append(append([]string{}, prefix...),
				"-i", "src.mkv", "-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "3",
				"-crf", "32", "-b:v", "0", "-row-mt", "1", "out.mkv"),
		},
		{
			name:   "an unregistered encoder falls back to the x264 shape",
			mutate: func(r *EncodeRequest) { r.Encoder = "not_a_real_codec" },
			want: append(append([]string{}, prefix...),
				"-i", "src.mkv", "-c:v", "not_a_real_codec", "-preset", "medium",
				"-crf", "23", "out.mkv"),
		},
		{
			name:   "extra params land after the codec slice",
			mutate: func(r *EncodeRequest) { r.ExtraParams = []string{"-threads", "4"} },
			want: append(append([]string{}, prefix...),
				"-i", "src.mkv", "-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"-threads", "4", "out.mkv"),
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			req := base
			tc.mutate(&req)
			if got := BuildFFmpegCommand(req, ""); !reflect.DeepEqual(got, tc.want) {
				t.Errorf("argv =\n %v\nwant\n %v", got, tc.want)
			}
		})
	}
}

// TestBuildVMAFCommand pins the libvmaf CLI argv, including the pix_fmt and
// bitdepth derivations.
func TestBuildVMAFCommand(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		req  ScoreRequest
		want []string
	}{
		{
			name: "defaults",
			req: ScoreRequest{
				Reference: "ref.yuv", Distorted: "dist.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p",
			},
			want: []string{
				"vmaf", "--reference", "ref.yuv", "--distorted", "dist.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1", "--json", "--output", "/tmp/vmaf.json",
			},
		},
		{
			name: "10-bit 4:2:2 with a backend and a sample-clip window",
			req: ScoreRequest{
				Reference: "ref.yuv", Distorted: "dist.yuv",
				Width: 3840, Height: 2160, PixFmt: "yuv422p10le",
				Backend: "cuda", FrameSkipRef: 120, FrameCnt: 240,
			},
			want: []string{
				"vmaf", "--reference", "ref.yuv", "--distorted", "dist.yuv",
				"--width", "3840", "--height", "2160",
				"--pixel_format", "422", "--bitdepth", "10",
				"--model", "version=vmaf_v0.6.1", "--json", "--output", "/tmp/vmaf.json",
				"--backend", "cuda", "--frame_skip_ref", "120", "--frame_cnt", "240",
			},
		},
		{
			name: "a pre-formatted model string passes through",
			req: ScoreRequest{
				Reference: "r", Distorted: "d", Width: 1, Height: 1,
				PixFmt: "yuv444p12le", Model: "path=/models/hdr.json",
			},
			want: []string{
				"vmaf", "--reference", "r", "--distorted", "d",
				"--width", "1", "--height", "1",
				"--pixel_format", "444", "--bitdepth", "12",
				"--model", "path=/models/hdr.json", "--json", "--output", "/tmp/vmaf.json",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := BuildVMAFCommand(tc.req, "/tmp/vmaf.json", "")
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("argv =\n %v\nwant\n %v", got, tc.want)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Version and score parsing.
// ---------------------------------------------------------------------------

func TestParseVersions(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		stderr      string
		encoder     string
		wantFFmpeg  string
		wantEncoder string
	}{
		{
			name:    "x264 banner",
			stderr:  "ffmpeg version 6.1.1 Copyright\nx264 - core 164 r3095 baee400\n",
			encoder: "libx264", wantFFmpeg: "6.1.1", wantEncoder: "libx264-164",
		},
		{
			name:    "defensive x264-core spelling",
			stderr:  "ffmpeg version n7.0\nx264-core 155\n",
			encoder: "libx264", wantFFmpeg: "n7.0", wantEncoder: "libx264-155",
		},
		{
			name:    "x265",
			stderr:  "ffmpeg version 6.1\nx265 [info]: HEVC encoder version 3.5+1\n",
			encoder: "libx265", wantFFmpeg: "6.1", wantEncoder: "libx265-3.5+1",
		},
		{
			name:    "svt-av1 modern banner",
			stderr:  "ffmpeg version 7.0\nSvt[info]:SVT-AV1 Encoder Lib v2.1.0\n",
			encoder: "libsvtav1", wantFFmpeg: "7.0", wantEncoder: "libsvtav1-2.1.0",
		},
		{
			name:    "libvpx",
			stderr:  "ffmpeg version 6.0\n[libvpx-vp9 @ 0x55f] v1.13.1\n",
			encoder: "libvpx-vp9", wantFFmpeg: "6.0", wantEncoder: "libvpx-vp9-1.13.1",
		},
		{
			name:    "libaom falls back to the adapter name on a quiet build",
			stderr:  "ffmpeg version 6.0\n",
			encoder: "libaom-av1", wantFFmpeg: "6.0", wantEncoder: "libaom-av1",
		},
		{
			name:    "libvvenc falls back to the adapter name",
			stderr:  "ffmpeg version 6.0\n",
			encoder: "libvvenc", wantFFmpeg: "6.0", wantEncoder: "libvvenc",
		},
		{
			name:    "hardware encoders return their token",
			stderr:  "ffmpeg version 6.1\n",
			encoder: "hevc_nvenc", wantFFmpeg: "6.1", wantEncoder: "hevc_nvenc",
		},
		{
			name:   "qsv token",
			stderr: "", encoder: "av1_qsv", wantFFmpeg: "unknown", wantEncoder: "av1_qsv",
		},
		{
			name:   "unknown encoder with no banner",
			stderr: "", encoder: "mystery", wantFFmpeg: "unknown", wantEncoder: "unknown",
		},
		{
			name:    "default auto-detect prefers x264 then x265",
			stderr:  "x265 [info]: HEVC encoder version 3.5\n",
			encoder: "", wantFFmpeg: "unknown", wantEncoder: "libx265-3.5",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			gotFFmpeg, gotEncoder := ParseVersions(tc.stderr, tc.encoder)
			if gotFFmpeg != tc.wantFFmpeg {
				t.Errorf("ffmpeg version = %q, want %q", gotFFmpeg, tc.wantFFmpeg)
			}
			if gotEncoder != tc.wantEncoder {
				t.Errorf("encoder version = %q, want %q", gotEncoder, tc.wantEncoder)
			}
		})
	}
}

func TestParseVMAFScore(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    float64
		wantOK  bool
	}{
		{
			name:    "modern pooled_metrics shape",
			payload: `{"pooled_metrics":{"vmaf":{"mean":93.25,"min":80.0}}}`,
			want:    93.25, wantOK: true,
		},
		{
			name:    "legacy top-level key",
			payload: `{"VMAF score": 88.5}`,
			want:    88.5, wantOK: true,
		},
		{
			name:    "neither shape",
			payload: `{"frames": []}`,
			wantOK:  false,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			var payload map[string]any
			if err := json.Unmarshal([]byte(tc.payload), &payload); err != nil {
				t.Fatalf("parse fixture: %v", err)
			}
			got, ok := ParseVMAFScore(payload)
			if ok != tc.wantOK {
				t.Fatalf("ok = %v, want %v", ok, tc.wantOK)
			}
			if ok && got != tc.want {
				t.Errorf("score = %v, want %v", got, tc.want)
			}
			if !ok && !math.IsNaN(got) {
				t.Errorf("a missing score should be NaN, got %v", got)
			}
		})
	}
}

// TestParseFeatureAggregates pins the integer_* key resolution and the
// documented absence semantics: a feature the model did not emit is simply
// missing, never zero.
func TestParseFeatureAggregates(t *testing.T) {
	t.Parallel()

	payload := map[string]any{
		"pooled_metrics": map[string]any{
			"integer_adm2":       map[string]any{"mean": 0.95, "stddev": 0.02},
			"integer_vif_scale0": map[string]any{"mean": 0.5, "harmonic_mean": 0.4},
			"motion2":            map[string]any{"mean": 3.5},
		},
	}
	means, stds := ParseFeatureAggregates(payload, Canonical6Features)

	if got, ok := means["adm2"]; !ok || got != 0.95 {
		t.Errorf("adm2 mean = %v (present=%v), want 0.95", got, ok)
	}
	if got, ok := stds["adm2"]; !ok || got != 0.02 {
		t.Errorf("adm2 stddev = %v (present=%v), want 0.02", got, ok)
	}
	if got, ok := means["vif_scale0"]; !ok || got != 0.5 {
		t.Errorf("vif_scale0 mean = %v (present=%v), want 0.5", got, ok)
	}
	if _, ok := stds["vif_scale0"]; ok {
		t.Error("a block without stddev must not produce a std entry")
	}
	if got, ok := means["motion2"]; !ok || got != 3.5 {
		t.Errorf("bare-name fallback failed: motion2 = %v (present=%v)", got, ok)
	}
	for _, absent := range []string{"vif_scale1", "vif_scale2", "vif_scale3"} {
		if _, ok := means[absent]; ok {
			t.Errorf("%s should be absent, not zero", absent)
		}
	}
}

// ---------------------------------------------------------------------------
// RunPlan.
// ---------------------------------------------------------------------------

// planWithCells builds a minimal auto.Plan carrying the fields the executor
// reads.
func planWithCells(cells ...map[string]any) auto.Plan {
	return auto.Plan{
		Cells: cells,
		Metadata: map[string]any{
			"source_meta": map[string]any{"width": 1280, "height": 720},
		},
	}
}

func cell(codec string, crf int, selected bool) map[string]any {
	return map[string]any{
		"codec": codec, "crf": crf, "selected": selected,
		"estimated_vmaf": 93.0, "estimated_bitrate_kbps": 4000.0,
		"prediction_source": "predictor",
	}
}

// scriptedRunner answers ffmpeg invocations with a fixed exit code and writes
// a canned libvmaf JSON for score invocations.
func scriptedRunner(t *testing.T, encodeExit, scoreExit int, vmafScore float64) (Runner, Runner) {
	t.Helper()

	encode := func(_ context.Context, argv []string) (CommandResult, error) {
		// RunEncode drives two different invocations through this seam: the
		// encode itself, and the `ffmpeg -version` fallback probe. Only the
		// former has an output path as its last argument — treating the probe
		// as an encode would write a file literally named "-version".
		if len(argv) == 2 && argv[1] == "-version" {
			return CommandResult{
				Stdout: "ffmpeg version 6.1.1\nconfiguration: --enable-libx264\n",
			}, nil
		}
		if encodeExit == 0 {
			// Materialise the output file so the size probe finds it.
			out := argv[len(argv)-1]
			if err := os.WriteFile(out, []byte("encoded-bytes"), 0o600); err != nil {
				t.Fatalf("stub encode write: %v", err)
			}
		}
		return CommandResult{
			Stderr:   "ffmpeg version 6.1.1\nx264 - core 164 r3095 baee400\n",
			ExitCode: encodeExit,
		}, nil
	}

	score := func(_ context.Context, argv []string) (CommandResult, error) {
		if scoreExit == 0 {
			var outPath string
			for i, a := range argv {
				if a == "--output" && i+1 < len(argv) {
					outPath = argv[i+1]
				}
			}
			payload := map[string]any{
				"pooled_metrics": map[string]any{
					"vmaf":         map[string]any{"mean": vmafScore},
					"integer_adm2": map[string]any{"mean": 0.97},
				},
			}
			raw, err := json.Marshal(payload)
			if err != nil {
				t.Fatalf("stub score marshal: %v", err)
			}
			if err := os.WriteFile(outPath, raw, 0o600); err != nil {
				t.Fatalf("stub score write: %v", err)
			}
		}
		return CommandResult{Stderr: "VMAF version 3.0.0\n", ExitCode: scoreExit}, nil
	}
	return encode, score
}

func TestRunPlanExecutesOnlyTheSelectedCell(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	plan := planWithCells(
		cell("libx264", 23, false),
		cell("libx265", 28, true),
		cell("libsvtav1", 35, false),
	)
	encode, score := scriptedRunner(t, 0, 0, 93.75)

	params := DefaultParams(dir)
	params.EncodeRunner, params.ScoreRunner, params.Log = encode, score, quietLogger()

	results, err := RunPlan(context.Background(), plan, "src.mkv", params)
	if err != nil {
		t.Fatalf("RunPlan: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("executed %d cells, want 1", len(results))
	}
	if got := results[0].Cell["codec"]; got != "libx265" {
		t.Errorf("executed codec %v, want the selected libx265", got)
	}
	if results[0].Score == nil || results[0].Score.VMAFScore != 93.75 {
		t.Errorf("score = %+v, want 93.75", results[0].Score)
	}
	if results[0].Encode == nil || results[0].Encode.SizeBytes == 0 {
		t.Error("the encode size probe did not run")
	}
}

func TestRunPlanExecuteAll(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	plan := planWithCells(
		cell("libx264", 23, false),
		cell("libx265", 28, true),
	)
	encode, score := scriptedRunner(t, 0, 0, 90.0)

	params := DefaultParams(dir)
	params.ExecuteAll = true
	params.EncodeRunner, params.ScoreRunner, params.Log = encode, score, quietLogger()

	results, err := RunPlan(context.Background(), plan, "src.mkv", params)
	if err != nil {
		t.Fatalf("RunPlan: %v", err)
	}
	if len(results) != 2 {
		t.Fatalf("executed %d cells, want 2", len(results))
	}
}

// TestRunPlanSkipsScoringAfterAFailedEncode pins the failure contract: the row
// is still written, carries the encode's non-zero status, and no score is
// attempted.
func TestRunPlanSkipsScoringAfterAFailedEncode(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	plan := planWithCells(cell("libx264", 23, true))
	encode, score := scriptedRunner(t, 1, 0, 90.0)

	params := DefaultParams(dir)
	params.EncodeRunner, params.ScoreRunner, params.Log = encode, score, quietLogger()

	results, err := RunPlan(context.Background(), plan, "src.mkv", params)
	if err != nil {
		t.Fatalf("RunPlan: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("executed %d cells, want 1", len(results))
	}
	if results[0].Score != nil {
		t.Error("scoring must be skipped after a failed encode")
	}
	if results[0].Row["encode_exit_status"] != 1 {
		t.Errorf("row encode_exit_status = %v, want 1", results[0].Row["encode_exit_status"])
	}
	if results[0].Row["vmaf_score"] != nil {
		t.Errorf("row vmaf_score = %v, want null", results[0].Row["vmaf_score"])
	}
}

// TestRunPlanWritesPortableJSONL checks the results log: one row per executed
// cell, appended, and free of the NaN / Infinity tokens the plan JSON allows —
// the JSONL goes through the strict emitter so downstream readers stay on
// RFC-8259.
func TestRunPlanWritesPortableJSONL(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	plan := planWithCells(cell("libx264", 23, true))
	encode, score := scriptedRunner(t, 0, 0, 93.0)

	params := DefaultParams(dir)
	params.EncodeRunner, params.ScoreRunner, params.Log = encode, score, quietLogger()

	// Two runs to prove the log appends rather than truncates.
	for i := 0; i < 2; i++ {
		if _, err := RunPlan(context.Background(), plan, "src.mkv", params); err != nil {
			t.Fatalf("RunPlan (run %d): %v", i, err)
		}
	}

	raw, err := os.ReadFile(filepath.Join(dir, ResultsFilename))
	if err != nil {
		t.Fatalf("read results log: %v", err)
	}
	lines := strings.Split(strings.TrimSpace(string(raw)), "\n")
	if len(lines) != 2 {
		t.Fatalf("results log has %d rows, want 2 (the second run must append)", len(lines))
	}
	for i, line := range lines {
		if strings.Contains(line, "NaN") || strings.Contains(line, "Infinity") {
			t.Errorf("row %d carries a non-portable token: %s", i, line)
		}
		var row map[string]any
		if err := json.Unmarshal([]byte(line), &row); err != nil {
			t.Fatalf("row %d is not valid JSON: %v", i, err)
		}
		for _, key := range []string{
			"codec", "crf", "selected", "estimated_vmaf", "encode_exit_status",
			"vmaf_score", "vmaf_binary_version", "feature_adm2_mean",
		} {
			if _, ok := row[key]; !ok {
				t.Errorf("row %d is missing key %q", i, key)
			}
		}
		// Plan cells carry no cell_index or preset; the Python executor's
		// defaults leave them null in the row.
		if row["cell_index"] != nil {
			t.Errorf("row %d cell_index = %v, want null", i, row["cell_index"])
		}
		if row["preset"] != nil {
			t.Errorf("row %d preset = %v, want null", i, row["preset"])
		}
	}
}

// TestRunPlanUsesPlanGeometry pins the documented override semantics: the
// plan's source_meta wins while the caller leaves the Python defaults in
// place, and loses as soon as the caller sets its own.
func TestRunPlanUsesPlanGeometry(t *testing.T) {
	t.Parallel()

	var seenScoreArgv []string
	captureScore := func(_ context.Context, argv []string) (CommandResult, error) {
		seenScoreArgv = argv
		return CommandResult{ExitCode: 1}, nil
	}

	tests := []struct {
		name         string
		width        int
		height       int
		wantW, wantH string
	}{
		{"plan geometry wins at the defaults", 1920, 1080, "1280", "720"},
		{"an explicit override wins", 640, 360, "640", "360"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			plan := planWithCells(cell("libx264", 23, true))
			encode, _ := scriptedRunner(t, 0, 0, 90)

			params := DefaultParams(dir)
			params.Width, params.Height = tc.width, tc.height
			params.EncodeRunner, params.ScoreRunner, params.Log = encode, captureScore, quietLogger()

			if _, err := RunPlan(context.Background(), plan, "src.mkv", params); err != nil {
				t.Fatalf("RunPlan: %v", err)
			}
			joined := strings.Join(seenScoreArgv, " ")
			if !strings.Contains(joined, "--width "+tc.wantW) {
				t.Errorf("score argv width: %s, want --width %s", joined, tc.wantW)
			}
			if !strings.Contains(joined, "--height "+tc.wantH) {
				t.Errorf("score argv height: %s, want --height %s", joined, tc.wantH)
			}
		})
	}
}

// TestProbeEncoderVersion covers the configure-summary fallback that recovers
// a stable encoder identifier when -hide_banner suppressed the per-encoder
// banner. The cache is keyed by (binary, encoder), so each case uses its own
// binary name to stay independent.
func TestProbeEncoderVersion(t *testing.T) {
	t.Parallel()

	configureLine := "ffmpeg version 6.1.1\nconfiguration: --prefix=/usr " +
		"--enable-libx264 --enable-libx265 --enable-libaom\n"

	tests := []struct {
		name    string
		bin     string
		encoder string
		stdout  string
		stderr  string
		want    string
	}{
		{
			name: "stdout configure line", bin: "/probe/a", encoder: "libx264",
			stdout: configureLine, want: "libx264-enabled",
		},
		{
			name: "stderr configure line (older builds)", bin: "/probe/b", encoder: "libx265",
			stderr: configureLine, want: "libx265-enabled",
		},
		{
			name: "codec not compiled in", bin: "/probe/c", encoder: "libvvenc",
			stdout: configureLine, want: "",
		},
		{
			name: "encoder with no probe pattern", bin: "/probe/d", encoder: "hevc_nvenc",
			stdout: configureLine, want: "",
		},
		{
			name: "libvpx matches the --enable-libvpx token", bin: "/probe/e",
			encoder: "libvpx-vp9",
			stdout:  "configuration: --enable-libvpx\n", want: "libvpx-vp9-enabled",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			calls := 0
			run := func(_ context.Context, argv []string) (CommandResult, error) {
				calls++
				if len(argv) != 2 || argv[1] != "-version" {
					t.Errorf("probe argv = %v, want [<bin> -version]", argv)
				}
				return CommandResult{Stdout: tc.stdout, Stderr: tc.stderr}, nil
			}
			got := ProbeEncoderVersion(context.Background(), tc.bin, tc.encoder, run)
			if got != tc.want {
				t.Errorf("label = %q, want %q", got, tc.want)
			}

			// A second call must be served from the cache. Encoders with no
			// probe pattern short-circuit before the runner, so they see zero.
			again := ProbeEncoderVersion(context.Background(), tc.bin, tc.encoder, run)
			if again != got {
				t.Errorf("cached label = %q, want %q", again, got)
			}
			if calls > 1 {
				t.Errorf("probe ran %d times; it must be cached per (binary, encoder)", calls)
			}
		})
	}
}

// TestRunEncodeReportsSpawnFailure covers the Runner error path, which is
// distinct from a non-zero exit.
func TestRunEncodeReportsSpawnFailure(t *testing.T) {
	t.Parallel()

	failing := func(_ context.Context, _ []string) (CommandResult, error) {
		return CommandResult{}, errors.New("exec: ffmpeg not found")
	}
	got := RunEncode(context.Background(), EncodeRequest{
		Source: "src.mkv", Encoder: "libx264", Preset: "medium", CRF: 23,
		Output: filepath.Join(t.TempDir(), "out.mkv"), SourceIsContainer: true,
	}, "ffmpeg", failing)

	if got.ExitStatus == 0 {
		t.Error("a spawn failure must report a non-zero exit status")
	}
	if !strings.Contains(got.StderrTail, "not found") {
		t.Errorf("the spawn error should reach StderrTail, got %q", got.StderrTail)
	}
}

// TestRunScoreTreatsCorruptJSONAsAScoringError pins the exit-65 convention:
// vmaf exiting 0 with unparseable JSON is a scoring failure, not a crash.
func TestRunScoreTreatsCorruptJSONAsAScoringError(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	corrupt := func(_ context.Context, argv []string) (CommandResult, error) {
		for i, a := range argv {
			if a == "--output" && i+1 < len(argv) {
				if err := os.WriteFile(argv[i+1], []byte("{partial"), 0o600); err != nil {
					t.Fatalf("write corrupt json: %v", err)
				}
			}
		}
		return CommandResult{Stderr: "VMAF version 3.0.0\n", ExitCode: 0}, nil
	}
	got := RunScore(context.Background(), ScoreRequest{
		Reference: "ref.yuv", Distorted: "dist.yuv", Width: 1920, Height: 1080,
		PixFmt: "yuv420p",
	}, "vmaf", dir, corrupt)

	if got.ExitStatus != 65 {
		t.Errorf("exit status = %d, want 65", got.ExitStatus)
	}
	if !math.IsNaN(got.VMAFScore) {
		t.Errorf("score = %v, want NaN", got.VMAFScore)
	}
	if got.BinaryVersion != "3.0.0" {
		t.Errorf("binary version = %q, want 3.0.0", got.BinaryVersion)
	}
}
