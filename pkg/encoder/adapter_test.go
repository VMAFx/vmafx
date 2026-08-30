// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Tests for the codec-adapter policy table. Every expected argv slice was
// produced by calling the corresponding Python adapter's
// ffmpeg_codec_args(preset, quality) in
// tools/vmaf-tune/src/vmaftune/codec_adapters/, so a mismatch here means the
// Go plan would emit a command the Python would not.

package encoder

import (
	"reflect"
	"strings"
	"testing"
)

func TestGetAdapter_KnownCodecs(t *testing.T) {
	t.Parallel()
	want := []string{
		"h264_amf", "h264_nvenc", "h264_qsv",
		"hevc_amf", "hevc_nvenc", "hevc_qsv",
		"libaom-av1", "libsvtav1", "libx264", "libx265",
	}
	if got := KnownAdapters(); !reflect.DeepEqual(got, want) {
		t.Errorf("KnownAdapters() = %v, want %v", got, want)
	}
	// The adapter table must cover exactly what NewExtended can construct;
	// an adapter without an encoder (or the reverse) is a latent runtime
	// failure in the per-shot planner.
	for _, name := range AllKnownEncoders() {
		if _, err := GetAdapter(name); err != nil {
			t.Errorf("encoder %q has no adapter: %v", name, err)
		}
	}
	for _, name := range KnownAdapters() {
		if _, err := NewExtended(name); err != nil {
			t.Errorf("adapter %q has no encoder: %v", name, err)
		}
	}
}

func TestGetAdapter_UnknownNamesThePythonFallback(t *testing.T) {
	t.Parallel()
	// The seven Python registry entries with no Go encoder must produce an
	// error that tells the operator where to go.
	for _, name := range []string{
		"av1_nvenc", "av1_qsv", "av1_amf",
		"h264_videotoolbox", "hevc_videotoolbox", "prores_videotoolbox",
		"libvvenc", "libvpx-vp9",
	} {
		_, err := GetAdapter(name)
		if err == nil {
			t.Fatalf("GetAdapter(%q) unexpectedly succeeded", name)
		}
		if !strings.Contains(err.Error(), "Python vmaf-tune") {
			t.Errorf("GetAdapter(%q) error should name the Python fallback, got: %v",
				name, err)
		}
	}
}

func TestAdapter_CodecArgs(t *testing.T) {
	t.Parallel()
	cases := []struct {
		codec   string
		preset  string
		quality int
		want    []string
	}{
		{"libx264", "medium", 23,
			[]string{"-c:v", "libx264", "-preset", "medium", "-crf", "23"}},
		{"libx264", "veryslow", 18,
			[]string{"-c:v", "libx264", "-preset", "veryslow", "-crf", "18"}},
		{"libx265", "medium", 28,
			[]string{"-c:v", "libx265", "-preset", "medium", "-crf", "28"}},
		// NVENC collapses ten mnemonics onto seven pN levels.
		{"h264_nvenc", "medium", 23,
			[]string{"-c:v", "h264_nvenc", "-preset", "p4", "-cq", "23"}},
		{"h264_nvenc", "ultrafast", 30,
			[]string{"-c:v", "h264_nvenc", "-preset", "p1", "-cq", "30"}},
		{"hevc_nvenc", "placebo", 20,
			[]string{"-c:v", "hevc_nvenc", "-preset", "p7", "-cq", "20"}},
		// QSV maps preset names identity-style and uses -global_quality.
		{"h264_qsv", "veryfast", 23,
			[]string{"-c:v", "h264_qsv", "-preset", "veryfast", "-global_quality", "23"}},
		{"hevc_qsv", "medium", 30,
			[]string{"-c:v", "hevc_qsv", "-preset", "medium", "-global_quality", "30"}},
		// AMF ignores -preset entirely: three quality rungs plus constant QP.
		{"h264_amf", "medium", 23, []string{
			"-c:v", "h264_amf", "-quality", "balanced",
			"-rc", "cqp", "-qp_i", "23", "-qp_p", "23",
		}},
		{"hevc_amf", "slow", 20, []string{
			"-c:v", "hevc_amf", "-quality", "quality",
			"-rc", "cqp", "-qp_i", "20", "-qp_p", "20",
		}},
		{"hevc_amf", "veryfast", 35, []string{
			"-c:v", "hevc_amf", "-quality", "speed",
			"-rc", "cqp", "-qp_i", "35", "-qp_p", "35",
		}},
		// SVT-AV1 takes an integer preset through the generic -preset flag.
		{"libsvtav1", "medium", 35,
			[]string{"-c:v", "libsvtav1", "-preset", "7", "-crf", "35"}},
		{"libsvtav1", "veryfast", 40,
			[]string{"-c:v", "libsvtav1", "-preset", "13", "-crf", "40"}},
		// libaom uses -cpu-used rather than -preset.
		{"libaom-av1", "medium", 35,
			[]string{"-c:v", "libaom-av1", "-cpu-used", "4", "-crf", "35"}},
		{"libaom-av1", "ultrafast", 45,
			[]string{"-c:v", "libaom-av1", "-cpu-used", "9", "-crf", "45"}},
	}
	for _, tc := range cases {
		t.Run(tc.codec+"/"+tc.preset, func(t *testing.T) {
			t.Parallel()
			a, err := GetAdapter(tc.codec)
			if err != nil {
				t.Fatalf("GetAdapter(%q): %v", tc.codec, err)
			}
			got, argErr := a.CodecArgs(tc.preset, tc.quality)
			if argErr != nil {
				t.Fatalf("CodecArgs: %v", argErr)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("CodecArgs(%q, %d) = %v, want %v",
					tc.preset, tc.quality, got, tc.want)
			}
		})
	}
}

func TestAdapter_CodecArgsRejectsUnknownPreset(t *testing.T) {
	t.Parallel()
	// "ultrafast" is an x264 preset but not a QSV one; "veryslow" is a QSV
	// preset but not an SVT-AV1 one. Both must be rejected rather than
	// silently passed through to ffmpeg.
	cases := []struct{ codec, preset string }{
		{"h264_qsv", "ultrafast"},
		{"libsvtav1", "veryslow"},
		{"libx264", "placebo"},
		{"libx264", "nonsense"},
	}
	for _, tc := range cases {
		a, err := GetAdapter(tc.codec)
		if err != nil {
			t.Fatalf("GetAdapter(%q): %v", tc.codec, err)
		}
		if _, argErr := a.CodecArgs(tc.preset, 23); argErr == nil {
			t.Errorf("%s.CodecArgs(%q) should reject the preset", tc.codec, tc.preset)
		}
	}
}

func TestAdapter_QualityWindows(t *testing.T) {
	t.Parallel()
	// Informative window (Python quality_range) vs. the absolute search
	// window the bisect uses (Python bisect._absolute_crf_range, ADR-0538).
	cases := []struct {
		codec                  string
		qLo, qHi               int
		absLo, absHi           int
		defaultQ               int
		defaultPreset, segment string
	}{
		{"libx264", 0, 51, 0, 51, 23, "medium", "medium"},
		{"libx265", 15, 40, 0, 51, 28, "medium", "medium"},
		{"h264_nvenc", 15, 40, 15, 40, 23, "medium", "medium"},
		{"h264_qsv", 1, 51, 1, 51, 23, "medium", "medium"},
		{"h264_amf", 15, 40, 15, 40, 23, "medium", "medium"},
		{"libsvtav1", 20, 50, 0, 63, 35, "medium", "medium"},
		{"libaom-av1", 0, 63, 0, 63, 35, "medium", "medium"},
	}
	for _, tc := range cases {
		t.Run(tc.codec, func(t *testing.T) {
			t.Parallel()
			a, err := GetAdapter(tc.codec)
			if err != nil {
				t.Fatalf("GetAdapter: %v", err)
			}
			if a.QualityLo != tc.qLo || a.QualityHi != tc.qHi {
				t.Errorf("quality window = (%d, %d), want (%d, %d)",
					a.QualityLo, a.QualityHi, tc.qLo, tc.qHi)
			}
			if a.AbsoluteLo != tc.absLo || a.AbsoluteHi != tc.absHi {
				t.Errorf("absolute window = (%d, %d), want (%d, %d)",
					a.AbsoluteLo, a.AbsoluteHi, tc.absLo, tc.absHi)
			}
			if a.QualityDefault != tc.defaultQ {
				t.Errorf("QualityDefault = %d, want %d", a.QualityDefault, tc.defaultQ)
			}
			if got := a.DefaultPreset(); got != tc.defaultPreset {
				t.Errorf("DefaultPreset() = %q, want %q", got, tc.defaultPreset)
			}
			if got := a.SegmentPreset(); got != tc.segment {
				t.Errorf("SegmentPreset() = %q, want %q", got, tc.segment)
			}
		})
	}
}

func TestAdapter_ClampAndValidate(t *testing.T) {
	t.Parallel()
	a, err := GetAdapter("libx265")
	if err != nil {
		t.Fatalf("GetAdapter: %v", err)
	}
	cases := []struct{ in, want int }{
		{3, 15}, {15, 15}, {28, 28}, {40, 40}, {51, 40},
	}
	for _, tc := range cases {
		if got := a.ClampQuality(tc.in); got != tc.want {
			t.Errorf("ClampQuality(%d) = %d, want %d", tc.in, got, tc.want)
		}
	}
	if err := a.Validate("medium", 28); err != nil {
		t.Errorf("Validate(medium, 28): %v", err)
	}
	if err := a.Validate("medium", 51); err == nil {
		t.Error("Validate should reject a quality outside the informative window")
	}
	if err := a.Validate("bogus", 28); err == nil {
		t.Error("Validate should reject an unknown preset")
	}
}

func TestAdapter_PresetFallbacksWithoutMedium(t *testing.T) {
	t.Parallel()
	// No shipped adapter lacks "medium", but the two fallbacks differ
	// (bisect._default_preset takes the middle entry, per_shot's segment
	// planner takes the first), so the distinction is pinned here.
	a := Adapter{Name: "synthetic", Presets: []string{"a", "b", "c", "d"}}
	if got := a.DefaultPreset(); got != "c" {
		t.Errorf("DefaultPreset() = %q, want the middle entry \"c\"", got)
	}
	if got := a.SegmentPreset(); got != "a" {
		t.Errorf("SegmentPreset() = %q, want the first entry \"a\"", got)
	}
	empty := Adapter{Name: "empty"}
	if got := empty.DefaultPreset(); got != "medium" {
		t.Errorf("DefaultPreset() with no presets = %q, want \"medium\"", got)
	}
	if got := empty.SegmentPreset(); got != "medium" {
		t.Errorf("SegmentPreset() with no presets = %q, want \"medium\"", got)
	}
}

func TestNewAdapterEncoder(t *testing.T) {
	t.Parallel()
	inputArgs := []string{"-f", "rawvideo", "-pix_fmt", "yuv420p"}
	enc, err := NewAdapterEncoder("libx265", "", inputArgs)
	if err != nil {
		t.Fatalf("NewAdapterEncoder: %v", err)
	}
	if enc.Name() != "libx265" {
		t.Errorf("Name() = %q, want libx265", enc.Name())
	}
	if enc.Preset() != "medium" {
		t.Errorf("empty preset should resolve to the adapter default, got %q", enc.Preset())
	}
	// The bisect must search the *absolute* window, not the informative one,
	// so a high VMAF target stays reachable (ADR-0538).
	if lo, hi := enc.CRFRange(); lo != 0 || hi != 51 {
		t.Errorf("CRFRange() = (%d, %d), want the absolute (0, 51)", lo, hi)
	}
	// The caller's slice must not be aliased into the encoder.
	inputArgs[1] = "mutated"
	if enc.inputArgs[1] != "rawvideo" {
		t.Error("NewAdapterEncoder aliased the caller's inputArgs slice")
	}

	if _, err := NewAdapterEncoder("libx264", "not-a-preset", nil); err == nil {
		t.Error("NewAdapterEncoder should reject an unknown preset")
	}
	if _, err := NewAdapterEncoder("av1_qsv", "", nil); err == nil {
		t.Error("NewAdapterEncoder should reject a codec with no Go encoder")
	}
}
