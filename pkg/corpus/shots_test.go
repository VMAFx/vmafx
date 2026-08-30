// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/shots_test.go — shot detection and metadata tests.
//
// The expected metadata was produced by vmaftune.per_shot.summarise_shots on
// the same shot lists. The standard deviation is compared bit-exactly: it lands
// in the corpus row's shot_duration_std_sec column verbatim, and
// statistics.pstdev's exact-rational variance is what pkg/corpus/pysum.go
// reproduces.

package corpus

import (
	"context"
	"os"
	"reflect"
	"testing"
)

func TestSummariseShots(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		shots     []Shot
		framerate float64
		want      ShotMetadata
	}{
		{
			name: "three shots of differing length",
			shots: []Shot{
				{StartFrame: 0, EndFrame: 24},
				{StartFrame: 24, EndFrame: 72},
				{StartFrame: 72, EndFrame: 96},
			},
			framerate: 24.0,
			want: ShotMetadata{
				Count:          3,
				AvgDurationSec: 1.3333333333333333,
				DurationStdSec: 0.4714045207910317,
			},
		},
		{
			// The population form keeps a singleton well-defined; the
			// sample form would emit NaN.
			name:      "a single real shot has zero spread",
			shots:     []Shot{{StartFrame: 0, EndFrame: 48}},
			framerate: 24.0,
			want:      ShotMetadata{Count: 1, AvgDurationSec: 2.0},
		},
		{
			// Shot{0, 1} is the sentinel detect_shots emits when it has no
			// frame count; it means "detection was not real".
			name:      "the single-shot sentinel reads as unavailable",
			shots:     []Shot{{StartFrame: 0, EndFrame: 1}},
			framerate: 24.0,
		},
		{name: "an empty list is unavailable", framerate: 24.0},
		{
			name:      "a zero framerate is unavailable",
			shots:     []Shot{{StartFrame: 0, EndFrame: 24}, {StartFrame: 24, EndFrame: 72}},
			framerate: 0.0,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := SummariseShots(tc.shots, tc.framerate)
			if got != tc.want {
				t.Errorf("SummariseShots() = %+v, want %+v", got, tc.want)
			}
		})
	}
}

func TestParsePerShotJSON(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    []Shot
		wantErr bool
	}{
		{
			// The source schema's end_frame is inclusive; the parser
			// normalises to the half-open convention.
			name:    "inclusive end frames become half-open",
			payload: `{"shots": [{"start_frame": 0, "end_frame": 23}, {"start_frame": 24, "end_frame": 71}]}`,
			want:    []Shot{{StartFrame: 0, EndFrame: 24}, {StartFrame: 24, EndFrame: 72}},
		},
		{
			name:    "an empty shot list yields the sentinel",
			payload: `{"shots": []}`,
			want:    []Shot{{StartFrame: 0, EndFrame: 1}},
		},
		{
			name:    "a payload without a shots key yields the sentinel",
			payload: `{}`,
			want:    []Shot{{StartFrame: 0, EndFrame: 1}},
		},
		{name: "unparseable JSON errors", payload: "not json", wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ParsePerShotJSON(tc.payload)
			if (err != nil) != tc.wantErr {
				t.Fatalf("ParsePerShotJSON error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("ParsePerShotJSON() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestShotLength(t *testing.T) {
	t.Parallel()

	if got := (Shot{StartFrame: 24, EndFrame: 72}).Length(); got != 48 {
		t.Errorf("Length() = %d, want 48", got)
	}
}

func TestBitdepthAwarePix(t *testing.T) {
	t.Parallel()

	tests := []struct{ pixFmt, want string }{
		{pixFmt: "yuv420p", want: "420"},
		{pixFmt: "yuv422p10le", want: "422"},
		{pixFmt: "yuv444p", want: "444"},
		{pixFmt: "p010le", want: "420"},
	}
	for _, tc := range tests {
		if got := bitdepthAwarePix(tc.pixFmt); got != tc.want {
			t.Errorf("bitdepthAwarePix(%q) = %q, want %q", tc.pixFmt, got, tc.want)
		}
	}
}

func TestSingleShotFallback(t *testing.T) {
	t.Parallel()

	if got := singleShotFallback(0); !reflect.DeepEqual(got,
		[]Shot{{StartFrame: 0, EndFrame: 1}}) {
		t.Errorf("singleShotFallback(0) = %v, want the sentinel", got)
	}
	if got := singleShotFallback(480); !reflect.DeepEqual(got,
		[]Shot{{StartFrame: 0, EndFrame: 480}}) {
		t.Errorf("singleShotFallback(480) = %v, want a whole-clip shot", got)
	}
}

func TestDetectShotsWithStatus(t *testing.T) {
	t.Parallel()

	opts := DetectShotsOptions{
		Width: 1920, Height: 1080, PixFmt: "yuv420p", TotalFrames: 480,
	}

	t.Run("a successful detection returns ok", func(t *testing.T) {
		t.Parallel()
		var argv []string
		stub := func(_ context.Context, cmd []string) RunResult {
			argv = cmd
			// The binary writes JSON to the path behind --output.
			for i, a := range cmd {
				if a == "--output" && i+1 < len(cmd) {
					body := `{"shots": [{"start_frame": 0, "end_frame": 23},` +
						`{"start_frame": 24, "end_frame": 479}]}`
					if err := os.WriteFile(cmd[i+1], []byte(body), 0o600); err != nil {
						return RunResult{ReturnCode: 1}
					}
				}
			}
			// The progress line goes to stdout, never the JSON.
			return RunResult{Stdout: "vmaf-perShot: wrote 2 shot(s) to ...\n"}
		}
		shots, ok := DetectShotsWithStatus(context.Background(), "/refs/clip.yuv", opts, stub)
		if !ok {
			t.Fatal("DetectShotsWithStatus reported failure on a successful run")
		}
		want := []Shot{{StartFrame: 0, EndFrame: 24}, {StartFrame: 24, EndFrame: 480}}
		if !reflect.DeepEqual(shots, want) {
			t.Errorf("shots = %v, want %v", shots, want)
		}
		for _, needle := range []string{"--reference", "--width", "--height",
			"--pixel_format", "--bitdepth", "--output", "--format"} {
			if !containsArg(argv, needle) {
				t.Errorf("vmaf-perShot argv is missing %q: %v", needle, argv)
			}
		}
		if !containsArg(argv, "json") {
			t.Errorf("vmaf-perShot argv should request the json format: %v", argv)
		}
	})

	t.Run("a diff-threshold override is threaded through", func(t *testing.T) {
		t.Parallel()
		threshold := 8.5
		withThreshold := opts
		withThreshold.DiffThreshold = &threshold
		var argv []string
		stub := func(_ context.Context, cmd []string) RunResult {
			argv = cmd
			return RunResult{ReturnCode: 1}
		}
		DetectShotsWithStatus(context.Background(), "/refs/clip.yuv", withThreshold, stub)
		found := false
		for i, a := range argv {
			if a == "--diff-threshold" && i+1 < len(argv) {
				found = true
				if argv[i+1] != "8.500000" {
					t.Errorf("--diff-threshold = %q, want 8.500000", argv[i+1])
				}
			}
		}
		if !found {
			t.Errorf("--diff-threshold was not forwarded: %v", argv)
		}
	})

	failures := []struct {
		name string
		stub Runner
	}{
		{
			name: "a non-zero exit falls back",
			stub: func(context.Context, []string) RunResult {
				return RunResult{ReturnCode: 1}
			},
		},
		{
			name: "an empty output file falls back",
			stub: func(context.Context, []string) RunResult { return RunResult{} },
		},
		{
			name: "unparseable JSON falls back",
			stub: func(_ context.Context, cmd []string) RunResult {
				for i, a := range cmd {
					if a == "--output" && i+1 < len(cmd) {
						if err := os.WriteFile(cmd[i+1], []byte("not json"), 0o600); err != nil {
							return RunResult{ReturnCode: 1}
						}
					}
				}
				return RunResult{}
			},
		},
	}
	for _, tc := range failures {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			shots, ok := DetectShotsWithStatus(context.Background(), "/refs/clip.yuv",
				opts, tc.stub)
			if ok {
				t.Error("DetectShotsWithStatus reported ok on a failed run")
			}
			// The fallback spans the whole clip, and SummariseShots maps a
			// not-ok result to the all-zero metadata regardless.
			if !reflect.DeepEqual(shots, []Shot{{StartFrame: 0, EndFrame: 480}}) {
				t.Errorf("fallback shots = %v, want a whole-clip shot", shots)
			}
		})
	}
}
