// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Tests for the raw-YUV ScoreFunc. The argv expectations mirror
// tools/vmaf-tune/src/vmaftune/score.py build_vmaf_command, whose flag order
// the fork's libvmaf CLI parser accepts verbatim.

package bisect

import (
	"github.com/VMAFx/vmafx/pkg/model"
	"math"
	"reflect"
	"strings"
	"testing"
)

func TestPixFmtToVMAF(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"yuv420p":     "420",
		"yuv420p10le": "420",
		"yuv422p":     "422",
		"yuv422p10le": "422",
		"yuv444p":     "444",
		"yuv444p12le": "444",
		// Unrecognised formats fall back to 420, matching the Python.
		"gbrp": "420",
		"":     "420",
	}
	for in, want := range cases {
		if got := pixFmtToVMAF(in); got != want {
			t.Errorf("pixFmtToVMAF(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestBitdepthFor(t *testing.T) {
	t.Parallel()
	cases := map[string]int{
		"yuv420p":     8,
		"yuv420p10le": 10,
		"yuv422p10le": 10,
		"yuv444p12le": 12,
		"p010le":      10,
		"gbrp":        8,
	}
	for in, want := range cases {
		if got := bitdepthFor(in); got != want {
			t.Errorf("bitdepthFor(%q) = %d, want %d", in, got, want)
		}
	}
}

func TestModelArg(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"vmaf_v0.6.1":            "version=vmaf_v0.6.1",
		"vmaf_v0.6.1neg":         "version=vmaf_v0.6.1neg",
		"path=/abs/model.json":   "path=/abs/model.json",
		"version=vmaf_4k_v0.6.1": "version=vmaf_4k_v0.6.1",
		// An empty model falls back to the production default (ADR-1169).
		"": "version=" + model.DefaultVersion,
	}
	for in, want := range cases {
		if got := modelArg(in); got != want {
			t.Errorf("modelArg(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestBuildVMAFCommand(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name   string
		params YUVScoreParams
		want   []string
	}{
		{
			name: "defaults omit the backend flag",
			params: YUVScoreParams{
				Width: 1920, Height: 1080, PixFmt: "yuv420p", Model: "vmaf_v0.6.1",
			},
			want: []string{
				"vmaf",
				"--reference", "ref.yuv",
				"--distorted", "dist.yuv",
				"--width", "1920",
				"--height", "1080",
				"--pixel_format", "420",
				"--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json",
				"--output", "out.json",
			},
		},
		{
			name: "backend and 10-bit geometry",
			params: YUVScoreParams{
				Width: 3840, Height: 2160, PixFmt: "yuv422p10le",
				Model: "vmaf_4k_v0.6.1neg", Backend: "cuda", VMAFBin: "/opt/vmaf",
			},
			want: []string{
				"/opt/vmaf",
				"--reference", "ref.yuv",
				"--distorted", "dist.yuv",
				"--width", "3840",
				"--height", "2160",
				"--pixel_format", "422",
				"--bitdepth", "10",
				"--model", "version=vmaf_4k_v0.6.1neg",
				"--json",
				"--output", "out.json",
				"--backend", "cuda",
			},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := BuildVMAFCommand("ref.yuv", "dist.yuv", "out.json", tc.params)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("BuildVMAFCommand =\n%v\nwant\n%v", got, tc.want)
			}
		})
	}
}

func TestParseVMAFJSON(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name    string
		payload string
		want    float64
		wantErr bool
	}{
		{
			name:    "modern pooled_metrics shape",
			payload: `{"pooled_metrics":{"vmaf":{"min":80.0,"max":95.0,"mean":88.5}}}`,
			want:    88.5,
		},
		{
			name:    "legacy top-level score",
			payload: `{"VMAF score": 91.25}`,
			want:    91.25,
		},
		{
			name:    "pooled_metrics wins over the legacy key",
			payload: `{"pooled_metrics":{"vmaf":{"mean":88.5}},"VMAF score":1.0}`,
			want:    88.5,
		},
		{
			name:    "zero is a real score, not a missing one",
			payload: `{"pooled_metrics":{"vmaf":{"mean":0.0}}}`,
			want:    0,
		},
		{
			name:    "missing mean errors",
			payload: `{"pooled_metrics":{"vmaf":{"min":1.0}}}`,
			wantErr: true,
		},
		{
			name:    "malformed JSON errors",
			payload: `{"pooled_metrics":`,
			wantErr: true,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ParseVMAFJSON([]byte(tc.payload))
			if tc.wantErr {
				if err == nil {
					t.Fatalf("ParseVMAFJSON(%s) = %v, want error", tc.payload, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("ParseVMAFJSON(%s): %v", tc.payload, err)
			}
			if got != tc.want {
				t.Errorf("ParseVMAFJSON = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestParseVMAFJSON_RejectsNonFinite(t *testing.T) {
	t.Parallel()
	// A corrupt score must be rejected at the source. Go's encoding/json
	// refuses bare NaN tokens outright; a huge exponent is the reachable
	// route to an infinite float64, and it must not propagate into a report
	// where json.Marshal would abort (cmd/vmafx-tune/AGENTS.md #2).
	_, err := ParseVMAFJSON([]byte(`{"pooled_metrics":{"vmaf":{"mean":1e400}}}`))
	if err == nil {
		t.Fatal("an overflowing mean should be rejected")
	}
	if !strings.Contains(err.Error(), "parse") && !strings.Contains(err.Error(), "non-finite") {
		t.Errorf("error should describe the parse/finiteness failure, got: %v", err)
	}
}

func TestRawYUVSuffixes(t *testing.T) {
	t.Parallel()
	// ADR-0499: .y4m is deliberately NOT a raw suffix. vmaf-tune always
	// passes explicit geometry, which routes both inputs through libvmaf's
	// raw_input_open, where a Y4M header trips the file-size guard.
	cases := map[string]bool{
		".yuv": true,
		"":     true,
		".y4m": false,
		".mkv": false,
		".mp4": false,
	}
	for ext, want := range cases {
		if got := rawYUVSuffixes[ext]; got != want {
			t.Errorf("rawYUVSuffixes[%q] = %v, want %v", ext, got, want)
		}
	}
}

func TestAcquire_NilSemaphoreIsNoOp(t *testing.T) {
	t.Parallel()
	release := acquire(nil)
	release() // must not panic or block
}

func TestAcquire_BoundsConcurrency(t *testing.T) {
	t.Parallel()
	sem := make(chan struct{}, 1)
	release := acquire(sem)
	if len(sem) != 1 {
		t.Fatalf("semaphore depth = %d, want 1 after acquire", len(sem))
	}
	release()
	if len(sem) != 0 {
		t.Errorf("semaphore depth = %d, want 0 after release", len(sem))
	}
}

func TestDecodeArgv(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name      string
		durationS float64
		want      []string
	}{
		{
			name:      "no duration clamp",
			durationS: 0,
			want: []string{
				"-y", "-hide_banner", "-loglevel", "error",
				"-i", "in.mkv",
				"-f", "rawvideo",
				"-pix_fmt", "yuv420p",
				"out.yuv",
			},
		},
		{
			// ADR-0498: "-t" after "-i" bounds the *output*, so a short
			// analysis window of a long source does not decode the whole file.
			name:      "duration clamp lands after the input",
			durationS: 2.5,
			want: []string{
				"-y", "-hide_banner", "-loglevel", "error",
				"-i", "in.mkv",
				"-f", "rawvideo",
				"-pix_fmt", "yuv420p",
				"-t", "2.5",
				"out.yuv",
			},
		},
		{
			// A NaN duration must degrade to a full decode, never emit an
			// unparseable "-t NaN".
			name:      "NaN duration omits the clamp",
			durationS: math.NaN(),
			want: []string{
				"-y", "-hide_banner", "-loglevel", "error",
				"-i", "in.mkv",
				"-f", "rawvideo",
				"-pix_fmt", "yuv420p",
				"out.yuv",
			},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := decodeArgv("in.mkv", "out.yuv", "yuv420p", tc.durationS)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("decodeArgv =\n%v\nwant\n%v", got, tc.want)
			}
		})
	}
}
