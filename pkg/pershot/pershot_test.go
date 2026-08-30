// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Table-driven tests for the per-shot tuner's pure logic: shot-range
// validation, the uniform-window splitter, the detector output parsers, the
// tuning clamp, plan construction, and the shot-metadata summary.
//
// Every expectation is transcribed from
// tools/vmaf-tune/src/vmaftune/per_shot.py, so a divergence here is a
// port regression rather than a taste difference.

package pershot

import (
	"context"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestNewShot(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name       string
		start, end int
		wantErr    bool
		wantLen    int
	}{
		{"single frame", 0, 1, false, 1},
		{"typical", 48, 96, false, 48},
		{"negative start", -1, 5, true, 0},
		{"inverted", 10, 5, true, 0},
		{"empty range", 7, 7, true, 0},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			shot, err := NewShot(tc.start, tc.end)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("NewShot(%d, %d) = %+v, want error", tc.start, tc.end, shot)
				}
				return
			}
			if err != nil {
				t.Fatalf("NewShot(%d, %d): %v", tc.start, tc.end, err)
			}
			if got := shot.Length(); got != tc.wantLen {
				t.Errorf("Length() = %d, want %d", got, tc.wantLen)
			}
		})
	}
}

func TestSplitLongShots(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name      string
		shots     []Shot
		maxSec    float64
		framerate float64
		want      []Shot
	}{
		{
			name:      "shorter than window is untouched",
			shots:     []Shot{{0, 24}},
			maxSec:    2.0,
			framerate: 24,
			want:      []Shot{{0, 24}},
		},
		{
			name:      "exact window is untouched",
			shots:     []Shot{{0, 48}},
			maxSec:    2.0,
			framerate: 24,
			want:      []Shot{{0, 48}},
		},
		{
			// 121 frames / 48-frame window = ceil(2.52) = 3 parts;
			// 121 = 3*40 + 1, so the first part carries the remainder.
			name:      "remainder spreads one frame at a time",
			shots:     []Shot{{0, 121}},
			maxSec:    2.0,
			framerate: 24,
			want:      []Shot{{0, 41}, {41, 81}, {81, 121}},
		},
		{
			// 100 frames / 24-frame window = ceil(4.17) = 5 equal parts of 20;
			// the trailing 10-frame shot is under the window and untouched.
			name:      "each shot split independently",
			shots:     []Shot{{0, 100}, {100, 110}},
			maxSec:    1.0,
			framerate: 24,
			want:      []Shot{{0, 20}, {20, 40}, {40, 60}, {60, 80}, {80, 100}, {100, 110}},
		},
		{
			name:      "zero window disables the splitter",
			shots:     []Shot{{0, 500}},
			maxSec:    0,
			framerate: 24,
			want:      []Shot{{0, 500}},
		},
		{
			name:      "non-positive framerate disables the splitter",
			shots:     []Shot{{0, 500}},
			maxSec:    2.0,
			framerate: 0,
			want:      []Shot{{0, 500}},
		},
		{
			name:      "NaN framerate disables the splitter",
			shots:     []Shot{{0, 500}},
			maxSec:    2.0,
			framerate: math.NaN(),
			want:      []Shot{{0, 500}},
		},
		{
			// A sub-frame window must still produce at least 1-frame parts.
			name:      "window rounds up to one frame",
			shots:     []Shot{{0, 3}},
			maxSec:    0.001,
			framerate: 24,
			want:      []Shot{{0, 1}, {1, 2}, {2, 3}},
		},
		{
			name:      "empty input",
			shots:     nil,
			maxSec:    2.0,
			framerate: 24,
			want:      nil,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := SplitLongShots(tc.shots, tc.maxSec, tc.framerate)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("SplitLongShots = %v, want %v", got, tc.want)
			}
			// Invariant: the partition must cover the original range exactly.
			if len(tc.shots) > 0 {
				total := 0
				for _, s := range tc.shots {
					total += s.Length()
				}
				split := 0
				for _, s := range got {
					split += s.Length()
				}
				if total != split {
					t.Errorf("split changed total frame count: %d -> %d", total, split)
				}
			}
		})
	}
}

func TestParseJSON(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name    string
		payload string
		want    []Shot
		wantErr bool
	}{
		{
			// The detector's end_frame is inclusive; the parser adds one.
			name:    "inclusive end normalised to half-open",
			payload: `{"shots":[{"start_frame":0,"end_frame":3},{"start_frame":4,"end_frame":47}]}`,
			want:    []Shot{{0, 4}, {4, 48}},
		},
		{
			name:    "extra columns ignored",
			payload: `{"shots":[{"shot_id":0,"start_frame":0,"end_frame":3,"frames":4,"predicted_crf":25.48}]}`,
			want:    []Shot{{0, 4}},
		},
		{
			name:    "empty shot array falls back to the sentinel",
			payload: `{"shots":[]}`,
			want:    []Shot{{0, 1}},
		},
		{
			name:    "missing shots key falls back to the sentinel",
			payload: `{"target_vmaf":90.0}`,
			want:    []Shot{{0, 1}},
		},
		{
			name:    "malformed JSON errors",
			payload: `{"shots":[`,
			wantErr: true,
		},
		{
			name:    "inverted range errors",
			payload: `{"shots":[{"start_frame":10,"end_frame":2}]}`,
			wantErr: true,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ParseJSON([]byte(tc.payload))
			if tc.wantErr {
				if err == nil {
					t.Fatalf("ParseJSON(%q) = %v, want error", tc.payload, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("ParseJSON(%q): %v", tc.payload, err)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("ParseJSON = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestParseCSV(t *testing.T) {
	t.Parallel()
	// Column layout copied from docs/usage/vmaf-perShot.md.
	const payload = "shot_id,start_frame,end_frame,frames,mean_complexity,mean_motion,predicted_crf\n" +
		"0,0,3,4,0.000051,0.020046,25.48\n" +
		"1,4,47,44,0.019353,0.016716,24.62\n"
	got, err := ParseCSV(payload)
	if err != nil {
		t.Fatalf("ParseCSV: %v", err)
	}
	want := []Shot{{0, 4}, {4, 48}}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ParseCSV = %v, want %v", got, want)
	}

	if _, err := ParseCSV("a,b\n1,2\n"); err == nil {
		t.Error("ParseCSV without start_frame/end_frame columns should error")
	}
	if shots, csvErr := ParseCSV(""); csvErr != nil || shots != nil {
		t.Errorf("ParseCSV(\"\") = (%v, %v), want (nil, nil)", shots, csvErr)
	}
}

func TestTune_ClampsIntoInformativeWindow(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name    string
		codec   string
		predCRF int
		want    int
	}{
		// libx264 informative window is the full 0..51, so nothing clamps.
		{"x264 in range", "libx264", 30, 30},
		// libx265's window is (15, 40).
		{"x265 clamps low", "libx265", 3, 15},
		{"x265 clamps high", "libx265", 48, 40},
		{"x265 in range", "libx265", 28, 28},
		// libsvtav1's window is (20, 50) even though it bisects over 0..63.
		{"svtav1 clamps low", "libsvtav1", 5, 20},
		{"svtav1 clamps high", "libsvtav1", 60, 50},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			recs, err := Tune([]Shot{{0, 24}}, TuneParams{
				TargetVMAF: 92,
				Encoder:    tc.codec,
				Predicate: func(Shot, float64, string) (int, float64, error) {
					return tc.predCRF, 92.5, nil
				},
			})
			if err != nil {
				t.Fatalf("Tune: %v", err)
			}
			if recs[0].CRF != tc.want {
				t.Errorf("CRF = %d, want %d", recs[0].CRF, tc.want)
			}
			if !math.IsNaN(recs[0].BitratekBps) {
				t.Errorf("BitratekBps = %v, want NaN before WithBitrates",
					recs[0].BitratekBps)
			}
		})
	}
}

func TestTune_Errors(t *testing.T) {
	t.Parallel()
	if _, err := Tune(nil, TuneParams{TargetVMAF: 92}); err == nil {
		t.Error("Tune with no shots should error")
	}
	if _, err := Tune([]Shot{{0, 5}}, TuneParams{
		TargetVMAF: 92, Encoder: "libvvenc",
	}); err == nil {
		t.Error("Tune with a codec outside the Go adapter table should error")
	}
	_, err := Tune([]Shot{{0, 5}}, TuneParams{
		TargetVMAF: 92,
		Predicate: func(Shot, float64, string) (int, float64, error) {
			return 0, 0, os.ErrPermission
		},
	})
	if err == nil || !strings.Contains(err.Error(), "[0, 5)") {
		t.Errorf("predicate error should name the shot range, got: %v", err)
	}
}

func TestTune_DefaultPredicateUsesCodecDefault(t *testing.T) {
	t.Parallel()
	// libx265's quality_default is 28, distinct from libx264's 23, so this
	// also proves the adapter (not a hardcoded constant) supplied the value.
	recs, err := Tune([]Shot{{0, 24}}, TuneParams{TargetVMAF: 88, Encoder: "libx265"})
	if err != nil {
		t.Fatalf("Tune: %v", err)
	}
	if recs[0].CRF != 28 {
		t.Errorf("default-predicate CRF = %d, want 28", recs[0].CRF)
	}
	if recs[0].PredictedVMAF != 88 {
		t.Errorf("default-predicate VMAF = %v, want the requested target 88",
			recs[0].PredictedVMAF)
	}
}

func TestWithBitrates(t *testing.T) {
	t.Parallel()
	recs := []Recommendation{
		{Shot: Shot{0, 24}, CRF: 22, BitratekBps: math.NaN()},
		{Shot: Shot{24, 48}, CRF: 25, BitratekBps: math.NaN()},
	}
	got := WithBitrates(recs, map[Shot]float64{{0, 24}: 1500.5})
	if got[0].BitratekBps != 1500.5 {
		t.Errorf("sidecar bitrate not applied: %v", got[0].BitratekBps)
	}
	if !math.IsNaN(got[1].BitratekBps) {
		t.Errorf("shot absent from the sidecar should keep NaN, got %v",
			got[1].BitratekBps)
	}
	// The input slice must not be mutated.
	if !math.IsNaN(recs[0].BitratekBps) {
		t.Error("WithBitrates mutated its input")
	}
}

func TestMerge_SegmentAndConcatCommands(t *testing.T) {
	t.Parallel()
	recs := []Recommendation{
		{Shot: Shot{0, 48}, CRF: 22, PredictedVMAF: 92.5},
		{Shot: Shot{48, 96}, CRF: 27, PredictedVMAF: 91.0},
	}
	plan, err := Merge(recs, MergeParams{
		Source:    "src.mp4",
		Output:    "out/per_shot_encode.mp4",
		Framerate: 24.0,
		Encoder:   "libx264",
	})
	if err != nil {
		t.Fatalf("Merge: %v", err)
	}

	wantFirst := []string{
		"ffmpeg", "-y", "-hide_banner",
		"-ss", "0.000000",
		"-i", "src.mp4",
		"-frames:v", "48",
		"-c:v", "libx264", "-preset", "medium", "-crf", "22",
		"out/segments/shot_0000.mp4",
	}
	if !reflect.DeepEqual(plan.SegmentCommands[0], wantFirst) {
		t.Errorf("segment[0] =\n%v\nwant\n%v", plan.SegmentCommands[0], wantFirst)
	}
	// Second shot starts at frame 48 => 2.000000 s at 24 fps.
	if plan.SegmentCommands[1][4] != "2.000000" {
		t.Errorf("segment[1] -ss = %q, want \"2.000000\"", plan.SegmentCommands[1][4])
	}

	wantConcat := []string{
		"ffmpeg", "-y", "-hide_banner",
		"-f", "concat", "-safe", "0",
		"-i", "out/segments/concat.txt",
		"-c", "copy",
		"out/per_shot_encode.mp4",
	}
	if !reflect.DeepEqual(plan.ConcatCommand, wantConcat) {
		t.Errorf("concat =\n%v\nwant\n%v", plan.ConcatCommand, wantConcat)
	}

	wantListing := "file 'out/segments/shot_0000.mp4'\nfile 'out/segments/shot_0001.mp4'\n"
	if plan.ConcatListing != wantListing {
		t.Errorf("listing = %q, want %q", plan.ConcatListing, wantListing)
	}
}

func TestMerge_CodecArgvIsAdapterSpecific(t *testing.T) {
	t.Parallel()
	// HP-1 / ADR-0297: the plan must carry each codec's own quality knob, not
	// a hardcoded "-preset medium -crf N" pair.
	cases := []struct {
		codec string
		want  []string
	}{
		{"libx264", []string{"-c:v", "libx264", "-preset", "medium", "-crf", "22"}},
		{"h264_nvenc", []string{"-c:v", "h264_nvenc", "-preset", "p4", "-cq", "22"}},
		{"h264_qsv", []string{"-c:v", "h264_qsv", "-preset", "medium", "-global_quality", "22"}},
		{"h264_amf", []string{
			"-c:v", "h264_amf", "-quality", "balanced",
			"-rc", "cqp", "-qp_i", "22", "-qp_p", "22",
		}},
		{"libsvtav1", []string{"-c:v", "libsvtav1", "-preset", "7", "-crf", "22"}},
		{"libaom-av1", []string{"-c:v", "libaom-av1", "-cpu-used", "4", "-crf", "22"}},
	}
	for _, tc := range cases {
		t.Run(tc.codec, func(t *testing.T) {
			t.Parallel()
			plan, err := Merge(
				[]Recommendation{{Shot: Shot{0, 24}, CRF: 22}},
				MergeParams{Source: "s.mp4", Output: "o.mp4", Framerate: 24, Encoder: tc.codec},
			)
			if err != nil {
				t.Fatalf("Merge: %v", err)
			}
			cmd := plan.SegmentCommands[0]
			// Codec argv sits between "-frames:v N" and the output path.
			got := cmd[9 : len(cmd)-1]
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("codec argv = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestMerge_Errors(t *testing.T) {
	t.Parallel()
	if _, err := Merge(nil, MergeParams{Framerate: 24}); err == nil {
		t.Error("Merge with no recommendations should error")
	}
	rec := []Recommendation{{Shot: Shot{0, 24}, CRF: 22}}
	for _, fps := range []float64{0, -1, math.NaN(), math.Inf(1)} {
		if _, err := Merge(rec, MergeParams{Framerate: fps}); err == nil {
			t.Errorf("Merge with framerate %v should error", fps)
		}
	}
	if _, err := Merge(rec, MergeParams{Framerate: 24, Encoder: "av1_nvenc"}); err == nil {
		t.Error("Merge with an unported codec should error")
	}
}

func TestSegmentDirFor(t *testing.T) {
	t.Parallel()
	cases := []struct{ segmentDir, output, want string }{
		{"", "per_shot_encode.mp4", "segments"},
		{"", "/a/b/out.mp4", "/a/b/segments"},
		{"/custom/segs", "/a/b/out.mp4", "/custom/segs"},
	}
	for _, tc := range cases {
		if got := SegmentDirFor(tc.segmentDir, tc.output); got != tc.want {
			t.Errorf("SegmentDirFor(%q, %q) = %q, want %q",
				tc.segmentDir, tc.output, got, tc.want)
		}
	}
}

func TestWriteConcatListing(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	plan := EncodingPlan{ConcatListing: "file 'a.mp4'\n"}
	path := filepath.Join(dir, "nested", "segments", "concat.txt")
	if err := WriteConcatListing(plan, path); err != nil {
		t.Fatalf("WriteConcatListing: %v", err)
	}
	data, err := os.ReadFile(path) //nolint:gosec // test-local temp path
	if err != nil {
		t.Fatalf("read listing: %v", err)
	}
	if string(data) != plan.ConcatListing {
		t.Errorf("listing = %q, want %q", data, plan.ConcatListing)
	}
}

func TestPlanToShellScript(t *testing.T) {
	t.Parallel()
	plan := EncodingPlan{
		SegmentCommands: [][]string{{"ffmpeg", "-i", "a"}, {"ffmpeg", "-i", "b"}},
		ConcatCommand:   []string{"ffmpeg", "-f", "concat"},
	}
	want := "#!/bin/sh\nset -eu\nffmpeg -i a\nffmpeg -i b\nffmpeg -f concat\n"
	if got := PlanToShellScript(plan); got != want {
		t.Errorf("PlanToShellScript = %q, want %q", got, want)
	}
}

func TestSummarise(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name      string
		shots     []Shot
		framerate float64
		want      Metadata
	}{
		{
			// Lengths 24 and 48 frames at 24 fps => 1 s and 2 s.
			// Mean 1.5, population std 0.5.
			name:      "two shots",
			shots:     []Shot{{0, 24}, {24, 72}},
			framerate: 24,
			want:      Metadata{Count: 2, AvgDurationSec: 1.5, DurationStdSec: 0.5},
		},
		{
			// Population std is 0 for a singleton, not NaN.
			name:      "real single shot",
			shots:     []Shot{{0, 48}},
			framerate: 24,
			want:      Metadata{Count: 1, AvgDurationSec: 2.0, DurationStdSec: 0},
		},
		{
			name:      "sentinel fallback list is treated as missing",
			shots:     []Shot{{0, 1}},
			framerate: 24,
			want:      Metadata{},
		},
		{
			name:      "empty",
			shots:     nil,
			framerate: 24,
			want:      Metadata{},
		},
		{
			name:      "non-positive framerate",
			shots:     []Shot{{0, 24}, {24, 72}},
			framerate: 0,
			want:      Metadata{},
		},
		{
			name:      "NaN framerate",
			shots:     []Shot{{0, 24}, {24, 72}},
			framerate: math.NaN(),
			want:      Metadata{},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := Summarise(tc.shots, tc.framerate)
			if got.Count != tc.want.Count ||
				math.Abs(got.AvgDurationSec-tc.want.AvgDurationSec) > 1e-12 ||
				math.Abs(got.DurationStdSec-tc.want.DurationStdSec) > 1e-12 {
				t.Errorf("Summarise = %+v, want %+v", got, tc.want)
			}
		})
	}
}

func TestPixFmtToDetector(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"yuv420p":     "420",
		"yuv420p10le": "420",
		"yuv422p":     "422",
		"yuv444p10le": "444",
		"gbrp":        "420", // unknown formats fall back to 420
	}
	for in, want := range cases {
		if got := pixFmtToDetector(in); got != want {
			t.Errorf("pixFmtToDetector(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestDetectShotsStatus_FallbackPaths(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name        string
		runner      func(context.Context, string, ...string) bool
		totalFrames int
		want        []Shot
		wantOK      bool
	}{
		{
			name:        "detector exits non-zero, frame count known",
			runner:      func(context.Context, string, ...string) bool { return false },
			totalFrames: 300,
			want:        []Shot{{0, 300}},
			wantOK:      false,
		},
		{
			name:        "detector exits non-zero, frame count unknown",
			runner:      func(context.Context, string, ...string) bool { return false },
			totalFrames: 0,
			want:        []Shot{{0, 1}},
			wantOK:      false,
		},
		{
			// A zero exit that leaves the output file empty is still a
			// fallback: the binary wrote nothing usable.
			name:        "detector exits zero but writes nothing",
			runner:      func(context.Context, string, ...string) bool { return true },
			totalFrames: 120,
			want:        []Shot{{0, 120}},
			wantOK:      false,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			shots, ok := DetectShotsStatus(context.Background(), "clip.yuv", DetectOptions{
				Width: 320, Height: 240, PixFmt: "yuv420p", Bitdepth: 8,
				TotalFrames: tc.totalFrames,
				Runner:      tc.runner,
			})
			if ok != tc.wantOK {
				t.Errorf("ok = %v, want %v", ok, tc.wantOK)
			}
			if !reflect.DeepEqual(shots, tc.want) {
				t.Errorf("shots = %v, want %v", shots, tc.want)
			}
		})
	}
}

func TestDetectShotsStatus_ParsesDetectorOutput(t *testing.T) {
	t.Parallel()
	// The stub writes the detector's JSON into the --output path the
	// implementation chose, then reports success — exercising the real
	// tempfile round-trip rather than only the fallback branches.
	var seenArgs []string
	runner := func(_ context.Context, _ string, args ...string) bool {
		seenArgs = args
		outPath := ""
		for i, a := range args {
			if a == "--output" && i+1 < len(args) {
				outPath = args[i+1]
			}
		}
		if outPath == "" {
			return false
		}
		payload := `{"shots":[{"start_frame":0,"end_frame":23},` +
			`{"start_frame":24,"end_frame":95}]}`
		return os.WriteFile(outPath, []byte(payload), 0o600) == nil
	}

	threshold := 8.5
	shots, ok := DetectShotsStatus(context.Background(), "clip.yuv", DetectOptions{
		Width: 1920, Height: 1080, PixFmt: "yuv422p10le", Bitdepth: 10,
		DiffThreshold: &threshold,
		Runner:        runner,
	})
	if !ok {
		t.Fatal("expected ok=true for a successful detector run")
	}
	want := []Shot{{0, 24}, {24, 96}}
	if !reflect.DeepEqual(shots, want) {
		t.Errorf("shots = %v, want %v", shots, want)
	}

	joined := strings.Join(seenArgs, " ")
	for _, want := range []string{
		"--reference clip.yuv",
		"--width 1920",
		"--height 1080",
		"--pixel_format 422",
		"--bitdepth 10",
		"--format json",
		"--diff-threshold 8.500000",
	} {
		if !strings.Contains(joined, want) {
			t.Errorf("detector argv missing %q; got: %s", want, joined)
		}
	}
}

func TestDetectShots_AppliesSplitter(t *testing.T) {
	t.Parallel()
	runner := func(_ context.Context, _ string, args ...string) bool {
		for i, a := range args {
			if a == "--output" && i+1 < len(args) {
				return os.WriteFile(args[i+1],
					[]byte(`{"shots":[{"start_frame":0,"end_frame":119}]}`), 0o600) == nil
			}
		}
		return false
	}
	// One detected 120-frame shot at 24 fps = 5 s; a 2 s window yields three
	// parts of 40 frames each.
	shots := DetectShots(context.Background(), "clip.yuv", DetectOptions{
		Width: 320, Height: 240, PixFmt: "yuv420p",
		Framerate: 24, MaxShotDurationSec: 2.0,
		Runner: runner,
	})
	want := []Shot{{0, 40}, {40, 80}, {80, 120}}
	if !reflect.DeepEqual(shots, want) {
		t.Errorf("DetectShots = %v, want %v", shots, want)
	}
}
