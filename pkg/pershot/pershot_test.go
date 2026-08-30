// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package pershot_test

import (
	"context"
	"errors"
	"os"
	"slices"
	"testing"

	"github.com/VMAFx/vmafx/pkg/pershot"
)

// TestShot covers the half-open range accessors and the validity guard.
func TestShot(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		shot       pershot.Shot
		wantLength int
		wantErr    bool
	}{
		{"ordinary range", pershot.Shot{StartFrame: 10, EndFrame: 40}, 30, false},
		{"single frame", pershot.Shot{StartFrame: 0, EndFrame: 1}, 1, false},
		{"empty range", pershot.Shot{StartFrame: 5, EndFrame: 5}, 0, true},
		{"inverted range", pershot.Shot{StartFrame: 5, EndFrame: 3}, -2, true},
		{"negative start", pershot.Shot{StartFrame: -1, EndFrame: 10}, 11, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := tc.shot.Length(); got != tc.wantLength {
				t.Errorf("Length = %d, want %d", got, tc.wantLength)
			}
			if err := tc.shot.Validate(); (err != nil) != tc.wantErr {
				t.Errorf("Validate error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestBuildCommand pins the vmaf-perShot argv, including the pix_fmt mapping
// and the optional --diff-threshold.
func TestBuildCommand(t *testing.T) {
	t.Parallel()

	threshold := 8.5
	tests := []struct {
		name string
		opts pershot.Options
		want []string
	}{
		{
			name: "defaults",
			opts: pershot.Options{Width: 1920, Height: 1080},
			want: []string{
				"vmaf-perShot",
				"--reference", "/src/a.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--output", "/tmp/shots.json",
				"--format", "json",
			},
		},
		{
			name: "422 10-bit with a custom binary",
			opts: pershot.Options{
				Width: 1280, Height: 720, PixFmt: "yuv422p10le",
				Bitdepth: 10, PerShotBin: "/opt/bin/vmaf-perShot",
			},
			want: []string{
				"/opt/bin/vmaf-perShot",
				"--reference", "/src/a.yuv",
				"--width", "1280", "--height", "720",
				"--pixel_format", "422", "--bitdepth", "10",
				"--output", "/tmp/shots.json",
				"--format", "json",
			},
		},
		{
			name: "444 with a diff-threshold override",
			opts: pershot.Options{
				Width: 640, Height: 480, PixFmt: "yuv444p",
				DiffThreshold: &threshold,
			},
			want: []string{
				"vmaf-perShot",
				"--reference", "/src/a.yuv",
				"--width", "640", "--height", "480",
				"--pixel_format", "444", "--bitdepth", "8",
				"--output", "/tmp/shots.json",
				"--format", "json",
				"--diff-threshold", "8.500000",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := pershot.BuildCommand("/src/a.yuv", "/tmp/shots.json", tc.opts)
			if !slices.Equal(got, tc.want) {
				t.Errorf("argv mismatch\n got: %v\nwant: %v", got, tc.want)
			}
		})
	}
}

// TestParseJSON asserts the inclusive->half-open conversion and the
// empty-list degradation.
func TestParseJSON(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    []pershot.Shot
		wantErr bool
	}{
		{
			name:    "inclusive end frames become half-open",
			payload: `{"shots":[{"start_frame":0,"end_frame":3},{"start_frame":4,"end_frame":9}]}`,
			want: []pershot.Shot{
				{StartFrame: 0, EndFrame: 4},
				{StartFrame: 4, EndFrame: 10},
			},
		},
		{
			name:    "empty shot list degrades to the sentinel",
			payload: `{"shots":[]}`,
			want:    []pershot.Shot{{StartFrame: 0, EndFrame: 1}},
		},
		{
			name:    "missing shots key degrades to the sentinel",
			payload: `{}`,
			want:    []pershot.Shot{{StartFrame: 0, EndFrame: 1}},
		},
		{
			name:    "malformed JSON is an error",
			payload: `{"shots":`,
			wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := pershot.ParseJSON([]byte(tc.payload))
			if (err != nil) != tc.wantErr {
				t.Fatalf("ParseJSON error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && !slices.Equal(got, tc.want) {
				t.Errorf("ParseJSON = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestParseCSV covers the sidecar variant, including a column order the
// header must drive rather than positional assumptions.
func TestParseCSV(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    []pershot.Shot
		wantErr bool
	}{
		{
			name:    "canonical column order",
			payload: "start_frame,end_frame\n0,3\n4,9\n",
			want: []pershot.Shot{
				{StartFrame: 0, EndFrame: 4},
				{StartFrame: 4, EndFrame: 10},
			},
		},
		{
			name:    "columns are resolved by header, not position",
			payload: "shot_id,end_frame,start_frame\n0,3,0\n1,9,4\n",
			want: []pershot.Shot{
				{StartFrame: 0, EndFrame: 4},
				{StartFrame: 4, EndFrame: 10},
			},
		},
		{
			name:    "header without the frame columns is an error",
			payload: "a,b\n1,2\n",
			wantErr: true,
		},
		{
			name:    "non-integer frames are an error",
			payload: "start_frame,end_frame\nx,3\n",
			wantErr: true,
		},
		{
			name:    "header only",
			payload: "start_frame,end_frame\n",
			want:    []pershot.Shot{},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := pershot.ParseCSV(tc.payload)
			if (err != nil) != tc.wantErr {
				t.Fatalf("ParseCSV error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && !slices.Equal(got, tc.want) {
				t.Errorf("ParseCSV = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestSplitLongShots pins the uniform splitter, including the at-most-one-
// frame partition imbalance and the no-op guards.
func TestSplitLongShots(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		shots     []pershot.Shot
		maxSec    float64
		framerate float64
		want      []pershot.Shot
	}{
		{
			name:   "shot within the window is untouched",
			shots:  []pershot.Shot{{StartFrame: 0, EndFrame: 48}},
			maxSec: 2.0, framerate: 24.0,
			want: []pershot.Shot{{StartFrame: 0, EndFrame: 48}},
		},
		{
			name:   "exact multiple splits evenly",
			shots:  []pershot.Shot{{StartFrame: 0, EndFrame: 96}},
			maxSec: 2.0, framerate: 24.0,
			want: []pershot.Shot{
				{StartFrame: 0, EndFrame: 48},
				{StartFrame: 48, EndFrame: 96},
			},
		},
		{
			name:   "remainder is spread one frame at a time",
			shots:  []pershot.Shot{{StartFrame: 0, EndFrame: 100}},
			maxSec: 2.0, framerate: 24.0,
			// ceil(100/48) = 3 parts; base 33, extra 1 -> 34, 33, 33.
			want: []pershot.Shot{
				{StartFrame: 0, EndFrame: 34},
				{StartFrame: 34, EndFrame: 67},
				{StartFrame: 67, EndFrame: 100},
			},
		},
		{
			name:   "zero window is a no-op",
			shots:  []pershot.Shot{{StartFrame: 0, EndFrame: 1000}},
			maxSec: 0, framerate: 24.0,
			want: []pershot.Shot{{StartFrame: 0, EndFrame: 1000}},
		},
		{
			name:   "zero framerate is a no-op",
			shots:  []pershot.Shot{{StartFrame: 0, EndFrame: 1000}},
			maxSec: 2.0, framerate: 0,
			want: []pershot.Shot{{StartFrame: 0, EndFrame: 1000}},
		},
		{
			name:  "empty input",
			shots: nil, maxSec: 2.0, framerate: 24.0,
			want: nil,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := pershot.SplitLongShots(tc.shots, tc.maxSec, tc.framerate)
			if !slices.Equal(got, tc.want) {
				t.Errorf("SplitLongShots = %v, want %v", got, tc.want)
			}
			// Partitions must tile the original range exactly.
			if len(tc.shots) == 1 && len(got) > 0 {
				if got[0].StartFrame != tc.shots[0].StartFrame ||
					got[len(got)-1].EndFrame != tc.shots[0].EndFrame {
					t.Errorf("partitions do not tile the original range: %v", got)
				}
			}
		})
	}
}

// TestSingleShotFallback pins the sentinel and the known-length forms.
func TestSingleShotFallback(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		totalFrames int
		want        []pershot.Shot
	}{
		{"unknown length emits the sentinel", 0, []pershot.Shot{{StartFrame: 0, EndFrame: 1}}},
		{"negative length emits the sentinel", -5, []pershot.Shot{{StartFrame: 0, EndFrame: 1}}},
		{"known length spans the clip", 1800, []pershot.Shot{{StartFrame: 0, EndFrame: 1800}}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := pershot.SingleShotFallback(tc.totalFrames)
			if !slices.Equal(got, tc.want) {
				t.Errorf("SingleShotFallback(%d) = %v, want %v",
					tc.totalFrames, got, tc.want)
			}
		})
	}
}

// TestIsFallback distinguishes the sentinel from a genuine one-shot source.
func TestIsFallback(t *testing.T) {
	t.Parallel()

	if !pershot.IsFallback([]pershot.Shot{{StartFrame: 0, EndFrame: 1}}) {
		t.Error("the [0, 1) sentinel should be reported as a fallback")
	}
	if pershot.IsFallback([]pershot.Shot{{StartFrame: 0, EndFrame: 1800}}) {
		t.Error("a genuine one-shot source is not a fallback")
	}
	if pershot.IsFallback(nil) {
		t.Error("an empty list is not the fallback sentinel")
	}
}

// TestDetect drives the detector seam: a clean run, a failing binary, an
// empty output file, and the splitter applied on top.
func TestDetect(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		payload    string
		writeFile  bool
		exitStatus int
		runErr     error
		opts       pershot.Options
		wantOK     bool
		wantShots  []pershot.Shot
	}{
		{
			name:      "clean detection",
			payload:   `{"shots":[{"start_frame":0,"end_frame":47},{"start_frame":48,"end_frame":95}]}`,
			writeFile: true,
			opts:      pershot.Options{Width: 1920, Height: 1080, TotalFrames: 96},
			wantOK:    true,
			wantShots: []pershot.Shot{
				{StartFrame: 0, EndFrame: 48},
				{StartFrame: 48, EndFrame: 96},
			},
		},
		{
			name: "non-zero exit falls back", writeFile: false, exitStatus: 1,
			opts:      pershot.Options{Width: 1920, Height: 1080, TotalFrames: 96},
			wantShots: []pershot.Shot{{StartFrame: 0, EndFrame: 96}},
		},
		{
			name: "spawn failure falls back", runErr: errors.New("no such binary"),
			opts:      pershot.Options{Width: 1920, Height: 1080, TotalFrames: 96},
			wantShots: []pershot.Shot{{StartFrame: 0, EndFrame: 96}},
		},
		{
			name: "empty output falls back", writeFile: true, payload: "   \n",
			opts:      pershot.Options{Width: 1920, Height: 1080, TotalFrames: 96},
			wantShots: []pershot.Shot{{StartFrame: 0, EndFrame: 96}},
		},
		{
			name: "unparseable output falls back", writeFile: true, payload: `{"shots":`,
			opts:      pershot.Options{Width: 1920, Height: 1080, TotalFrames: 0},
			wantShots: []pershot.Shot{{StartFrame: 0, EndFrame: 1}},
		},
		{
			name:      "splitter runs on top of a clean detection",
			payload:   `{"shots":[{"start_frame":0,"end_frame":95}]}`,
			writeFile: true,
			opts: pershot.Options{
				Width: 1920, Height: 1080, TotalFrames: 96,
				MaxShotDurationSec: 2.0, Framerate: 24.0,
			},
			wantOK: true,
			wantShots: []pershot.Shot{
				{StartFrame: 0, EndFrame: 48},
				{StartFrame: 48, EndFrame: 96},
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			runner := func(_ context.Context, argv []string) (int, error) {
				if tc.writeFile {
					for i := range argv {
						if argv[i] == "--output" && i+1 < len(argv) {
							if err := os.WriteFile(
								argv[i+1], []byte(tc.payload), 0o600); err != nil {
								t.Errorf("stub write: %v", err)
							}
						}
					}
				}
				return tc.exitStatus, tc.runErr
			}
			got, ok := pershot.Detect(
				context.Background(), "/src/a.yuv", tc.opts, runner)
			if ok != tc.wantOK {
				t.Errorf("detection ok = %v, want %v", ok, tc.wantOK)
			}
			if !slices.Equal(got, tc.wantShots) {
				t.Errorf("shots = %v, want %v", got, tc.wantShots)
			}
		})
	}
}
