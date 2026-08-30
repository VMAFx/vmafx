// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package codecadapter_test

import (
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
)

// TestFFmpegCodecArgs_matchesPythonGolden walks every (codec, preset) pair in
// the Python-derived golden table and asserts the Go registry emits the same
// argv slice. This is the parity gate for the whole registry.
func TestFFmpegCodecArgs_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	for codec, presets := range goldenArgv {
		meta, ok := goldenMeta[codec]
		if !ok {
			t.Fatalf("golden table inconsistency: %s missing from goldenMeta", codec)
		}
		for preset, want := range presets {
			t.Run(codec+"/"+preset, func(t *testing.T) {
				t.Parallel()

				a, err := codecadapter.Get(codec)
				if err != nil {
					t.Fatalf("Get(%q): %v", codec, err)
				}
				got, argErr := a.FFmpegCodecArgs(preset, meta.QualityDefault)
				if argErr != nil {
					t.Fatalf("FFmpegCodecArgs(%q, %d): %v", preset, meta.QualityDefault, argErr)
				}
				if !slices.Equal(got, want) {
					t.Errorf("argv mismatch\n got: %v\nwant: %v", got, want)
				}
			})
		}
	}
}

// TestProbeArgs_matchesPythonGolden pins the predictor probe-encode argv.
func TestProbeArgs_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	for codec, want := range goldenProbeArgv {
		t.Run(codec, func(t *testing.T) {
			t.Parallel()

			a, err := codecadapter.Get(codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", codec, err)
			}
			got, argErr := a.ProbeArgs()
			if argErr != nil {
				t.Fatalf("ProbeArgs: %v", argErr)
			}
			if !slices.Equal(got, want) {
				t.Errorf("probe argv mismatch\n got: %v\nwant: %v", got, want)
			}
		})
	}
}

// TestAdapterMetadata_matchesPythonGolden pins the scalar contract fields the
// CLI surfaces (quality windows, preset lists, capability flags).
func TestAdapterMetadata_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	for codec, want := range goldenMeta {
		t.Run(codec, func(t *testing.T) {
			t.Parallel()

			a, err := codecadapter.Get(codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", codec, err)
			}
			switch {
			case a.QualityKnob != want.QualityKnob:
				t.Errorf("QualityKnob = %q, want %q", a.QualityKnob, want.QualityKnob)
			case a.QualityRange[0] != want.QualityLo || a.QualityRange[1] != want.QualityHi:
				t.Errorf("QualityRange = %v, want [%d %d]",
					a.QualityRange, want.QualityLo, want.QualityHi)
			case a.QualityDefault != want.QualityDefault:
				t.Errorf("QualityDefault = %d, want %d", a.QualityDefault, want.QualityDefault)
			case !slices.Equal(a.Presets, want.Presets):
				t.Errorf("Presets = %v, want %v", a.Presets, want.Presets)
			case a.ProbePreset != want.ProbePreset:
				t.Errorf("ProbePreset = %q, want %q", a.ProbePreset, want.ProbePreset)
			case a.ProbeQuality != want.ProbeQuality:
				t.Errorf("ProbeQuality = %d, want %d", a.ProbeQuality, want.ProbeQuality)
			case a.SupportsQPFile != want.SupportsQPFile:
				t.Errorf("SupportsQPFile = %v, want %v", a.SupportsQPFile, want.SupportsQPFile)
			case a.SupportsEncoderStats != want.SupportsStats:
				t.Errorf("SupportsEncoderStats = %v, want %v",
					a.SupportsEncoderStats, want.SupportsStats)
			case a.SupportsTwoPass != want.SupportsTwoPass:
				t.Errorf("SupportsTwoPass = %v, want %v", a.SupportsTwoPass, want.SupportsTwoPass)
			}
		})
	}
}

// TestKnown_matchesPythonRegistry pins the sorted codec list the CLI uses for
// its --codec / --encoder choices.
func TestKnown_matchesPythonRegistry(t *testing.T) {
	t.Parallel()

	want := []string{
		"av1_amf", "av1_nvenc", "av1_qsv", "av1_videotoolbox",
		"h264_amf", "h264_nvenc", "h264_qsv", "h264_videotoolbox",
		"hevc_amf", "hevc_nvenc", "hevc_qsv", "hevc_videotoolbox",
		"libaom-av1", "libsvtav1", "libvpx-vp9", "libvvenc",
		"libx264", "libx265", "prores_videotoolbox",
	}
	if got := codecadapter.Known(); !slices.Equal(got, want) {
		t.Errorf("Known() = %v, want %v", got, want)
	}
}

// TestGet_unknownCodec asserts the error path names the codec and lists the
// registry, matching the Python KeyError message shape.
func TestGet_unknownCodec(t *testing.T) {
	t.Parallel()

	_, err := codecadapter.Get("libx266")
	if err == nil {
		t.Fatal("expected error for unknown codec")
	}
	if !strings.Contains(err.Error(), "libx266") ||
		!strings.Contains(err.Error(), "libx264") {
		t.Errorf("error should name the bad codec and list known ones; got %q", err)
	}
}

// TestAV1VideoToolbox_unavailable pins the ADR-0339 hard-fail: the adapter is
// registered (so --encoder accepts the name and the message is actionable) but
// every argv call errors.
func TestAV1VideoToolbox_unavailable(t *testing.T) {
	t.Parallel()

	a, err := codecadapter.Get("av1_videotoolbox")
	if err != nil {
		t.Fatalf("av1_videotoolbox should be registered: %v", err)
	}
	if _, argErr := a.FFmpegCodecArgs("medium", 50); argErr == nil {
		t.Error("expected FFmpegCodecArgs to fail for av1_videotoolbox")
	}
	if _, probeErr := a.ProbeArgs(); probeErr == nil {
		t.Error("expected ProbeArgs to fail for av1_videotoolbox")
	}
}

// TestValidate exercises the preset / quality-window guard.
func TestValidate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		wantErr bool
	}{
		{"x264 ok", "libx264", "medium", 23, false},
		{"x264 crf floor", "libx264", "medium", 0, false},
		{"x264 crf ceiling", "libx264", "medium", 51, false},
		{"x264 crf over", "libx264", "medium", 52, true},
		{"x264 crf under", "libx264", "medium", -1, true},
		{"x264 bad preset", "libx264", "turbo", 23, true},
		{"x265 narrow window low", "libx265", "medium", 14, true},
		{"x265 narrow window ok", "libx265", "medium", 15, false},
		{"qsv has no ultrafast", "h264_qsv", "ultrafast", 23, true},
		{"qsv veryfast ok", "h264_qsv", "veryfast", 23, false},
		{"svtav1 has no ultrafast", "libsvtav1", "ultrafast", 35, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			a, err := codecadapter.Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			gotErr := a.Validate(tc.preset, tc.quality)
			if (gotErr != nil) != tc.wantErr {
				t.Errorf("Validate(%q, %d) error = %v, wantErr %v",
					tc.preset, tc.quality, gotErr, tc.wantErr)
			}
		})
	}
}

// TestTwoPassArgs covers the single-pass short circuit, the supported path and
// the unsupported-encoder rejection.
func TestTwoPassArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		pass    int
		want    []string
		wantErr bool
	}{
		{"single pass is a no-op", "libx264", 0, nil, false},
		{"pass 1", "libx264", 1, []string{"-pass", "1", "-passlogfile", "/tmp/s"}, false},
		{"pass 2", "libx264", 2, []string{"-pass", "2", "-passlogfile", "/tmp/s"}, false},
		{"pass 3 rejected", "libx264", 3, nil, true},
		{"nvenc has no 2-pass", "h264_nvenc", 1, nil, true},
		{"nvenc single pass still fine", "h264_nvenc", 0, nil, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			a, err := codecadapter.Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			got, gotErr := a.TwoPassArgs(tc.pass, "/tmp/s")
			if (gotErr != nil) != tc.wantErr {
				t.Fatalf("TwoPassArgs(%d) error = %v, wantErr %v", tc.pass, gotErr, tc.wantErr)
			}
			if !tc.wantErr && !slices.Equal(got, tc.want) {
				t.Errorf("TwoPassArgs(%d) = %v, want %v", tc.pass, got, tc.want)
			}
		})
	}
}

// TestResolveCodecArgs covers the extra-params layering the encode driver
// depends on, including the documented AMF de-duplication deviation.
func TestResolveCodecArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		want    []string
	}{
		{
			name:  "libvpx appends row-mt after the codec args",
			codec: "libvpx-vp9", preset: "medium", quality: 32,
			want: []string{
				"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "3",
				"-crf", "32", "-b:v", "0", "-row-mt", "1",
			},
		},
		{
			name:  "libx264 has no extra params",
			codec: "libx264", preset: "medium", quality: 23,
			want: []string{"-c:v", "libx264", "-preset", "medium", "-crf", "23"},
		},
		{
			// The Python original emits the -quality/-rc/-qp_i/-qp_p run twice
			// here; see the ResolveCodecArgs doc comment.
			name:  "AMF tokens are emitted once, not duplicated",
			codec: "h264_amf", preset: "medium", quality: 23,
			want: []string{
				"-c:v", "h264_amf", "-quality", "balanced",
				"-rc", "cqp", "-qp_i", "23", "-qp_p", "23",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			a, err := codecadapter.Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			got, argErr := a.ResolveCodecArgs(tc.preset, tc.quality)
			if argErr != nil {
				t.Fatalf("ResolveCodecArgs: %v", argErr)
			}
			if !slices.Equal(got, tc.want) {
				t.Errorf("ResolveCodecArgs = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestGOPArgs covers the keyint emission contract.
func TestGOPArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		keyint    int
		minKeyint int
		want      []string
	}{
		{"zero keyint leaves the default", 0, 0, nil},
		{"keyint only", 48, 0, []string{"-g", "48"}},
		{"keyint and min", 48, 24, []string{"-g", "48", "-keyint_min", "24"}},
	}
	a, err := codecadapter.Get("libx264")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := a.GOPArgs(tc.keyint, tc.minKeyint); !slices.Equal(got, tc.want) {
				t.Errorf("GOPArgs(%d, %d) = %v, want %v",
					tc.keyint, tc.minKeyint, got, tc.want)
			}
		})
	}
}
