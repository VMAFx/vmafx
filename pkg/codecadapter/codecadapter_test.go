// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/codecadapter/codecadapter_test.go — argv-parity tests for the Go port of
// vmaftune.codec_adapters.
//
// Every `want` argv below was captured by running
// vmaftune.encode._resolve_codec_args against the live Python adapter for that
// (codec, preset, quality) triple. During development the whole matrix was
// compared — 19 codecs x every declared preset x quality in {0, 5, 23, 51},
// 696 cases — with zero mismatches; the table here keeps one case per codec
// plus every interesting preset projection as the regression gate.

package codecadapter

import (
	"errors"
	"reflect"
	"testing"
)

func TestResolvedArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		want    []string
	}{
		{
			name:  "libx264 uses generic preset and crf",
			codec: "libx264", preset: "medium", quality: 23,
			want: []string{"-c:v", "libx264", "-preset", "medium", "-crf", "23"},
		},
		{
			name:  "libx265 shares the x264 shape",
			codec: "libx265", preset: "placebo", quality: 28,
			want: []string{"-c:v", "libx265", "-preset", "placebo", "-crf", "28"},
		},
		{
			// SVT-AV1's FFmpeg wrapper takes an integer through -preset.
			name:  "libsvtav1 projects preset name onto an integer",
			codec: "libsvtav1", preset: "medium", quality: 35,
			want: []string{"-c:v", "libsvtav1", "-preset", "7", "-crf", "35"},
		},
		{
			name:  "libsvtav1 fastest preset is 13",
			codec: "libsvtav1", preset: "veryfast", quality: 40,
			want: []string{"-c:v", "libsvtav1", "-preset", "13", "-crf", "40"},
		},
		{
			// libaom dials -cpu-used rather than -preset.
			name:  "libaom-av1 projects preset onto cpu-used",
			codec: "libaom-av1", preset: "ultrafast", quality: 35,
			want: []string{"-c:v", "libaom-av1", "-cpu-used", "9", "-crf", "35"},
		},
		{
			// libvpx caps -cpu-used at 5 and appends row multithreading.
			name:  "libvpx-vp9 pins deadline, VBR and row-mt",
			codec: "libvpx-vp9", preset: "medium", quality: 31,
			want: []string{
				"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "3",
				"-crf", "31", "-b:v", "0", "-row-mt", "1",
			},
		},
		{
			name:  "libvpx-vp9 clamps the fast end at cpu-used 5",
			codec: "libvpx-vp9", preset: "ultrafast", quality: 31,
			want: []string{
				"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5",
				"-crf", "31", "-b:v", "0", "-row-mt", "1",
			},
		},
		{
			// VVenC dials -qp and collapses ten mnemonics onto five natives.
			name:  "libvvenc collapses onto a native preset and dials qp",
			codec: "libvvenc", preset: "placebo", quality: 32,
			want: []string{"-c:v", "libvvenc", "-preset", "slower", "-qp", "32"},
		},
		{
			name:  "libvvenc maps ultrafast onto its fastest native preset",
			codec: "libvvenc", preset: "ultrafast", quality: 32,
			want: []string{"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
		},
		{
			// NVENC uses -cq and its own pN preset ladder.
			name:  "h264_nvenc collapses medium onto p4",
			codec: "h264_nvenc", preset: "medium", quality: 23,
			want: []string{"-c:v", "h264_nvenc", "-preset", "p4", "-cq", "23"},
		},
		{
			name:  "nvenc clamps the fast end at p1",
			codec: "hevc_nvenc", preset: "ultrafast", quality: 23,
			want: []string{"-c:v", "hevc_nvenc", "-preset", "p1", "-cq", "23"},
		},
		{
			name:  "nvenc clamps the slow end at p7",
			codec: "av1_nvenc", preset: "placebo", quality: 23,
			want: []string{"-c:v", "av1_nvenc", "-preset", "p7", "-cq", "23"},
		},
		{
			// QSV's preset projection is the identity over its vocabulary.
			name:  "h264_qsv dials global_quality",
			codec: "h264_qsv", preset: "veryfast", quality: 23,
			want: []string{"-c:v", "h264_qsv", "-preset", "veryfast", "-global_quality", "23"},
		},
		{
			name:  "av1_qsv shares the QSV shape",
			codec: "av1_qsv", preset: "veryslow", quality: 30,
			want: []string{"-c:v", "av1_qsv", "-preset", "veryslow", "-global_quality", "30"},
		},
		{
			// AMF: constant-QP block instead of -preset, emitted TWICE. See
			// amfExtraParams for why the duplicate is deliberate.
			name:  "h264_amf emits the cqp block twice, mirroring CPython",
			codec: "h264_amf", preset: "medium", quality: 23,
			want: []string{
				"-c:v", "h264_amf",
				"-quality", "balanced", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23",
				"-quality", "balanced", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23",
			},
		},
		{
			name:  "amf collapses slow presets onto the quality rung",
			codec: "hevc_amf", preset: "placebo", quality: 30,
			want: []string{
				"-c:v", "hevc_amf",
				"-quality", "quality", "-rc", "cqp", "-qp_i", "30", "-qp_p", "30",
				"-quality", "quality", "-rc", "cqp", "-qp_i", "30", "-qp_p", "30",
			},
		},
		{
			name:  "amf collapses fast presets onto the speed rung",
			codec: "av1_amf", preset: "ultrafast", quality: 30,
			want: []string{
				"-c:v", "av1_amf",
				"-quality", "speed", "-rc", "cqp", "-qp_i", "30", "-qp_p", "30",
				"-quality", "speed", "-rc", "cqp", "-qp_i", "30", "-qp_p", "30",
			},
		},
		{
			// VideoToolbox has a binary speed dial, not a preset ladder.
			name:  "h264_videotoolbox maps fast presets onto realtime 1",
			codec: "h264_videotoolbox", preset: "fast", quality: 50,
			want: []string{"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "50"},
		},
		{
			name:  "hevc_videotoolbox maps quality presets onto realtime 0",
			codec: "hevc_videotoolbox", preset: "veryslow", quality: 60,
			want: []string{"-c:v", "hevc_videotoolbox", "-realtime", "0", "-q:v", "60"},
		},
		{
			// ProRes has no quality scalar: the slot carries the tier id.
			name:  "prores_videotoolbox emits the named tier alias",
			codec: "prores_videotoolbox", preset: "medium", quality: 3,
			want: []string{
				"-c:v", "prores_videotoolbox", "-realtime", "0", "-profile:v", "hq",
			},
		},
		{
			name:  "prores tier 0 is proxy",
			codec: "prores_videotoolbox", preset: "ultrafast", quality: 0,
			want: []string{
				"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "proxy",
			},
		},
		{
			name:  "prores tier 5 is xq",
			codec: "prores_videotoolbox", preset: "slow", quality: 5,
			want: []string{
				"-c:v", "prores_videotoolbox", "-realtime", "0", "-profile:v", "xq",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			a, err := Get(tt.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tt.codec, err)
			}
			got, err := a.ResolvedArgs(tt.preset, tt.quality)
			if err != nil {
				t.Fatalf("ResolvedArgs(%q, %d): %v", tt.preset, tt.quality, err)
			}
			if !reflect.DeepEqual(got, tt.want) {
				t.Errorf("ResolvedArgs(%q, %d) =\n %v\nwant\n %v",
					tt.preset, tt.quality, got, tt.want)
			}
		})
	}
}

func TestResolvedArgsRejectsUnknownPreset(t *testing.T) {
	t.Parallel()

	// Each codec has at least one name outside its own vocabulary; the Python
	// adapters reject those rather than substituting a default.
	tests := []struct{ codec, preset string }{
		{"libx264", "placebo"},           // x265-only rung
		{"libx265", "slowest"},           // NVENC/AMF-only rung
		{"libsvtav1", "ultrafast"},       // not in SVT-AV1's eight
		{"libaom-av1", "veryslow"},       // libaom uses "slowest", not "veryslow"
		{"libvpx-vp9", "veryslow"},       //
		{"libvvenc", "veryslow"},         //
		{"h264_nvenc", "veryslow"},       // NVENC uses "slowest"
		{"h264_qsv", "ultrafast"},        // QSV tops out at "veryfast"
		{"h264_amf", "veryslow"},         //
		{"h264_videotoolbox", "placebo"}, // VT shares the x264 nine only
		{"prores_videotoolbox", "slowest"},
	}

	for _, tt := range tests {
		t.Run(tt.codec+"/"+tt.preset, func(t *testing.T) {
			t.Parallel()
			a, err := Get(tt.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tt.codec, err)
			}
			if got, err := a.ResolvedArgs(tt.preset, 23); err == nil {
				t.Errorf("ResolvedArgs(%q) accepted an out-of-vocabulary preset: %v",
					tt.preset, got)
			}
		})
	}
}

func TestProResRejectsOutOfRangeTier(t *testing.T) {
	t.Parallel()

	a, err := Get("prores_videotoolbox")
	if err != nil {
		t.Fatal(err)
	}
	for _, tier := range []int{-1, 6, 99} {
		if _, err := a.ResolvedArgs("medium", tier); err == nil {
			t.Errorf("tier %d was accepted; want an error", tier)
		}
	}
}

// TestAV1VideoToolboxFailsClosed pins the ADR-0339 placeholder behaviour:
// upstream FFmpeg ships no av1_videotoolbox encoder, so the adapter refuses to
// emit an argv shape it cannot verify rather than guessing one.
func TestAV1VideoToolboxFailsClosed(t *testing.T) {
	a, err := Get("av1_videotoolbox")
	if err != nil {
		t.Fatal(err)
	}

	if _, err := a.ResolvedArgs("medium", 50); !errors.Is(err, ErrAV1VideoToolboxUnavailable) {
		t.Errorf("ResolvedArgs err = %v, want ErrAV1VideoToolboxUnavailable", err)
	}

	// When a caller has confirmed the encoder exists, the adapter emits the
	// same shape as the other VideoToolbox codecs.
	orig := AV1VideoToolboxAvailable
	t.Cleanup(func() { AV1VideoToolboxAvailable = orig })
	AV1VideoToolboxAvailable = func() bool { return true }

	got, err := a.ResolvedArgs("medium", 50)
	if err != nil {
		t.Fatalf("ResolvedArgs after probe success: %v", err)
	}
	want := []string{"-c:v", "av1_videotoolbox", "-realtime", "0", "-q:v", "50"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ResolvedArgs = %v, want %v", got, want)
	}
}

func TestKnownMatchesPythonRegistry(t *testing.T) {
	t.Parallel()

	// codec_adapters.known_codecs(), sorted, as of the port.
	want := []string{
		"av1_amf", "av1_nvenc", "av1_qsv", "av1_videotoolbox",
		"h264_amf", "h264_nvenc", "h264_qsv", "h264_videotoolbox",
		"hevc_amf", "hevc_nvenc", "hevc_qsv", "hevc_videotoolbox",
		"libaom-av1", "libsvtav1", "libvpx-vp9", "libvvenc",
		"libx264", "libx265", "prores_videotoolbox",
	}
	if got := Known(); !reflect.DeepEqual(got, want) {
		t.Errorf("Known() =\n %v\nwant\n %v", got, want)
	}
}

func TestGetRejectsUnknownCodec(t *testing.T) {
	t.Parallel()

	if _, err := Get("libtheora"); err == nil {
		t.Fatal("Get accepted an unregistered codec")
	}
}

func TestDefaultPreset(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		codec string
		want  string
	}{
		{"registered codec with medium", "libx264", "medium"},
		{"x265 also has medium", "libx265", "medium"},
		{"svtav1 has medium", "libsvtav1", "medium"},
		{"qsv has medium", "h264_qsv", "medium"},
		{"unregistered codec falls back", "libtheora", "medium"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			if got := DefaultPreset(tt.codec); got != tt.want {
				t.Errorf("DefaultPreset(%q) = %q, want %q", tt.codec, got, tt.want)
			}
		})
	}
}

// TestResolveCodecArgsFallsBackForUnknownCodec pins encode._resolve_codec_args'
// forgiving behaviour: an encoder outside the registry still produces the
// historic libx264-shaped argv rather than failing the encode outright.
func TestResolveCodecArgsFallsBackForUnknownCodec(t *testing.T) {
	t.Parallel()

	got, err := ResolveCodecArgs("libtheora", "medium", 23)
	if err != nil {
		t.Fatalf("ResolveCodecArgs: %v", err)
	}
	want := []string{"-c:v", "libtheora", "-preset", "medium", "-crf", "23"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ResolveCodecArgs = %v, want %v", got, want)
	}
}

func TestResolveCodecArgsUsesRegistryWhenKnown(t *testing.T) {
	t.Parallel()

	got, err := ResolveCodecArgs("h264_nvenc", "medium", 23)
	if err != nil {
		t.Fatalf("ResolveCodecArgs: %v", err)
	}
	want := []string{"-c:v", "h264_nvenc", "-preset", "p4", "-cq", "23"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ResolveCodecArgs = %v, want %v", got, want)
	}
}
