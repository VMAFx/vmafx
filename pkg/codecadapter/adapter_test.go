// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/codecadapter/adapter_test.go — argv-shape and validation parity tests.
//
// Every expected argv slice here was read off the corresponding Python adapter
// under tools/vmaf-tune/src/vmaftune/codec_adapters/, because the encode driver
// splices these slices straight into the ffmpeg command line: a drift here
// silently changes what the corpus sweep actually encodes.

package codecadapter

import (
	"errors"
	"reflect"
	"testing"
)

func TestFFmpegCodecArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		want    []string
	}{
		{
			name:  "libx264 uses -preset and -crf verbatim",
			codec: "libx264", preset: "medium", quality: 23,
			want: []string{"-c:v", "libx264", "-preset", "medium", "-crf", "23"},
		},
		{
			name:  "libx265 mirrors the x264 shape",
			codec: "libx265", preset: "slow", quality: 28,
			want: []string{"-c:v", "libx265", "-preset", "slow", "-crf", "28"},
		},
		{
			name:  "libaom-av1 uses -cpu-used instead of -preset",
			codec: "libaom-av1", preset: "medium", quality: 35,
			want: []string{"-c:v", "libaom-av1", "-cpu-used", "4", "-crf", "35"},
		},
		{
			name:  "libvpx-vp9 pins the good deadline and VBR-0",
			codec: "libvpx-vp9", preset: "medium", quality: 32,
			want: []string{
				"-c:v", "libvpx-vp9", "-deadline", "good",
				"-cpu-used", "3", "-crf", "32", "-b:v", "0",
			},
		},
		{
			name:  "libsvtav1 renders the preset as an integer",
			codec: "libsvtav1", preset: "medium", quality: 35,
			want: []string{"-c:v", "libsvtav1", "-preset", "7", "-crf", "35"},
		},
		{
			name:  "libvvenc compresses onto the native preset and uses -qp",
			codec: "libvvenc", preset: "ultrafast", quality: 32,
			want: []string{"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
		},
		{
			name:  "h264_nvenc collapses the mnemonic onto pN and uses -cq",
			codec: "h264_nvenc", preset: "slower", quality: 23,
			want: []string{"-c:v", "h264_nvenc", "-preset", "p6", "-cq", "23"},
		},
		{
			name:  "hevc_qsv identity-maps the preset and uses -global_quality",
			codec: "hevc_qsv", preset: "veryfast", quality: 23,
			want: []string{"-c:v", "hevc_qsv", "-preset", "veryfast", "-global_quality", "23"},
		},
		{
			name:  "av1_amf uses the 3-rung quality dial and constant-QP",
			codec: "av1_amf", preset: "medium", quality: 23,
			want: []string{
				"-c:v", "av1_amf", "-quality", "balanced",
				"-rc", "cqp", "-qp_i", "23", "-qp_p", "23",
			},
		},
		{
			name:  "h264_videotoolbox maps the preset onto -realtime",
			codec: "h264_videotoolbox", preset: "slow", quality: 60,
			want: []string{"-c:v", "h264_videotoolbox", "-realtime", "0", "-q:v", "60"},
		},
		{
			name:  "prores_videotoolbox emits the named tier alias",
			codec: "prores_videotoolbox", preset: "ultrafast", quality: 3,
			want: []string{
				"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "hq",
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			a, err := Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			got, err := a.FFmpegCodecArgs(tc.preset, tc.quality)
			if err != nil {
				t.Fatalf("FFmpegCodecArgs: %v", err)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("FFmpegCodecArgs() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestExtraParams(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		want    []string
	}{
		{name: "libx264 has none", codec: "libx264", preset: "medium", quality: 23},
		{name: "libvvenc default knobs emit nothing", codec: "libvvenc", preset: "medium", quality: 32},
		{
			name:  "libvpx-vp9 enables row multithreading",
			codec: "libvpx-vp9", preset: "medium", quality: 32,
			want: []string{"-row-mt", "1"},
		},
		{
			// Parity note: the Python AMF adapter's extra_params takes
			// (preset, qp) and repeats the block ffmpeg_codec_args already
			// emitted. The port reproduces the repetition on purpose.
			name:  "h264_amf repeats the constant-QP block",
			codec: "h264_amf", preset: "medium", quality: 23,
			want: []string{"-quality", "balanced", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			a, err := Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			got := a.ExtraParams(tc.preset, tc.quality)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("ExtraParams() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestValidate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		wantErr bool
	}{
		{name: "x264 accepts its full 0..51 window", codec: "libx264", preset: "medium", quality: 51},
		{name: "x264 rejects crf 52", codec: "libx264", preset: "medium", quality: 52, wantErr: true},
		{name: "x264 rejects an unknown preset", codec: "libx264", preset: "turbo", quality: 23, wantErr: true},
		{name: "x264 has no placebo preset", codec: "libx264", preset: "placebo", quality: 23, wantErr: true},
		{name: "x265 does have placebo", codec: "libx265", preset: "placebo", quality: 28},
		{name: "x265 window is narrower than x264's", codec: "libx265", preset: "medium", quality: 14, wantErr: true},
		{name: "nvenc validates against the hardware window not the sweep window",
			codec: "h264_nvenc", preset: "medium", quality: 5},
		{name: "nvenc rejects cq 52", codec: "h264_nvenc", preset: "medium", quality: 52, wantErr: true},
		{name: "qsv rejects global_quality 0", codec: "h264_qsv", preset: "medium", quality: 0, wantErr: true},
		{name: "qsv has no ultrafast preset", codec: "h264_qsv", preset: "ultrafast", quality: 23, wantErr: true},
		{name: "svtav1 rejects crf above its absolute range",
			codec: "libsvtav1", preset: "medium", quality: 64, wantErr: true},
		{name: "svtav1 rejects crf inside the absolute range but outside Phase A",
			codec: "libsvtav1", preset: "medium", quality: 10, wantErr: true},
		{name: "prores validates the tier id", codec: "prores_videotoolbox", preset: "medium", quality: 6, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			a, err := Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			gotErr := a.Validate(tc.preset, tc.quality)
			if (gotErr != nil) != tc.wantErr {
				t.Errorf("Validate(%q, %d) error = %v, wantErr = %v",
					tc.preset, tc.quality, gotErr, tc.wantErr)
			}
		})
	}
}

func TestTwoPassArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		pass    int
		want    []string
		wantErr bool
	}{
		{name: "x264 pass 0 is single-pass", codec: "libx264", pass: 0},
		{
			name:  "x264 pass 1 uses the generic passlogfile pair",
			codec: "libx264", pass: 1,
			want: []string{"-pass", "1", "-passlogfile", "/tmp/stats"},
		},
		{
			name:  "x265 routes pass control through -x265-params",
			codec: "libx265", pass: 2,
			want: []string{"-x265-params", "pass=2:stats=/tmp/stats"},
		},
		{name: "x264 rejects pass 3", codec: "libx264", pass: 3, wantErr: true},
		{
			name:  "nvenc pass 1 is the in-encoder multipass flag",
			codec: "h264_nvenc", pass: 1,
			want: []string{"-multipass", "fullres"},
		},
		{name: "nvenc pass 2 is a no-op", codec: "h264_nvenc", pass: 2},
		{
			name:  "qsv pass 1 is the look-ahead flag set",
			codec: "h264_qsv", pass: 1,
			want: []string{"-extbrc", "1", "-look_ahead_depth", "40"},
		},
		{
			name:  "amf pass 1 is the pre-analysis flag",
			codec: "h264_amf", pass: 1,
			want: []string{"-preanalysis", "true"},
		},
		{
			name:  "videotoolbox always refuses",
			codec: "h264_videotoolbox", pass: 1, wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			a, err := Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			got, gotErr := a.TwoPassArgs(tc.pass, "/tmp/stats")
			if (gotErr != nil) != tc.wantErr {
				t.Fatalf("TwoPassArgs(%d) error = %v, wantErr = %v", tc.pass, gotErr, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("TwoPassArgs(%d) = %v, want %v", tc.pass, got, tc.want)
			}
		})
	}
}

func TestVideoToolboxTwoPassErrorIsIdentifiable(t *testing.T) {
	t.Parallel()

	a, err := Get("hevc_videotoolbox")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	_, gotErr := a.TwoPassArgs(1, "/tmp/stats")
	if !errors.Is(gotErr, ErrVideoToolboxTwoPassUnsupported) {
		t.Errorf("TwoPassArgs error = %v, want it to wrap ErrVideoToolboxTwoPassUnsupported",
			gotErr)
	}
}

func TestAV1VideoToolboxStaysInactiveUntilProbed(t *testing.T) {
	// Not parallel: it swaps the package-level activation hook.
	a, err := Get("av1_videotoolbox")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if vErr := a.Validate("medium", 60); !errors.Is(vErr, ErrAV1VideoToolboxUnavailable) {
		t.Fatalf("Validate on the inactive placeholder = %v, want ErrAV1VideoToolboxUnavailable",
			vErr)
	}

	AV1VideoToolboxAvailable = func() bool { return true }
	t.Cleanup(func() { AV1VideoToolboxAvailable = nil })

	if vErr := a.Validate("medium", 60); vErr != nil {
		t.Fatalf("Validate on the activated placeholder = %v, want nil", vErr)
	}
	got, argsErr := a.FFmpegCodecArgs("medium", 60)
	if argsErr != nil {
		t.Fatalf("FFmpegCodecArgs: %v", argsErr)
	}
	want := []string{"-c:v", "av1_videotoolbox", "-realtime", "0", "-q:v", "60"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("FFmpegCodecArgs() = %v, want %v", got, want)
	}
}

func TestGopArgs(t *testing.T) {
	t.Parallel()

	minKeyint := 12
	tooLarge := 100

	tests := []struct {
		name      string
		keyint    int
		minKeyint *int
		want      []string
		wantErr   bool
	}{
		{name: "keyint only", keyint: 48, want: []string{"-g", "48"}},
		{
			name: "keyint plus min", keyint: 48, minKeyint: &minKeyint,
			want: []string{"-g", "48", "-keyint_min", "12"},
		},
		{name: "keyint below 1 is rejected", keyint: 0, wantErr: true},
		{name: "min above keyint is rejected", keyint: 48, minKeyint: &tooLarge, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := DefaultGopArgs(tc.keyint, tc.minKeyint)
			if (err != nil) != tc.wantErr {
				t.Fatalf("DefaultGopArgs error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("DefaultGopArgs() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestForceKeyframesArgs(t *testing.T) {
	t.Parallel()

	if got := DefaultForceKeyframesArgs(nil); got != nil {
		t.Errorf("DefaultForceKeyframesArgs(nil) = %v, want nil", got)
	}
	got := DefaultForceKeyframesArgs([]float64{0, 1.5, 2.25})
	want := []string{"-force_key_frames", "0.000000,1.500000,2.250000"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("DefaultForceKeyframesArgs() = %v, want %v", got, want)
	}

	// NVENC needs -forced-idr on top or it may emit non-IDR keyframes.
	a, err := Get("hevc_nvenc")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	gotNVENC := a.ForceKeyframesArgs([]float64{1.0})
	wantNVENC := []string{"-force_key_frames", "1.000000", "-forced-idr", "1"}
	if !reflect.DeepEqual(gotNVENC, wantNVENC) {
		t.Errorf("nvenc ForceKeyframesArgs() = %v, want %v", gotNVENC, wantNVENC)
	}
}

func TestRegistry(t *testing.T) {
	t.Parallel()

	// The registry must carry exactly the codecs the Python _REGISTRY does;
	// the corpus CLI restricts --encoder to this list.
	want := []string{
		"av1_amf", "av1_nvenc", "av1_qsv", "av1_videotoolbox",
		"h264_amf", "h264_nvenc", "h264_qsv", "h264_videotoolbox",
		"hevc_amf", "hevc_nvenc", "hevc_qsv", "hevc_videotoolbox",
		"libaom-av1", "libsvtav1", "libvpx-vp9", "libvvenc",
		"libx264", "libx265", "prores_videotoolbox",
	}
	if got := KnownCodecs(); !reflect.DeepEqual(got, want) {
		t.Errorf("KnownCodecs() = %v, want %v", got, want)
	}
	if _, err := Get("libx999"); err == nil {
		t.Error("Get on an unknown codec should error")
	}
	for _, name := range want {
		a, err := Get(name)
		if err != nil {
			t.Fatalf("Get(%q): %v", name, err)
		}
		if a.Name() != name {
			t.Errorf("registry key %q maps to adapter named %q", name, a.Name())
		}
		lo, hi := a.QualityRange()
		if lo > hi {
			t.Errorf("%s: quality range [%d, %d] is inverted", name, lo, hi)
		}
		def := a.QualityDefault()
		if def < lo || def > hi {
			t.Errorf("%s: quality default %d is outside [%d, %d]", name, def, lo, hi)
		}
		if len(a.Presets()) == 0 {
			t.Errorf("%s: declares no presets", name)
		}
	}
}

func TestParseAvailableCodecs(t *testing.T) {
	t.Parallel()

	const encodersOutput = `Encoders:
 V..... = Video
 ------
 V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC
 V....D libx265              libx265 H.265 / HEVC
 V..... h264_nvenc           NVIDIA NVENC H.264 encoder
 A..... aac                  AAC (Advanced Audio Coding)
 V..... some_unknown_codec   Not in the registry
`

	got := ParseAvailableCodecs(encodersOutput, true)
	want := map[string]bool{"libx264": true, "libx265": true, "h264_nvenc": true}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ParseAvailableCodecs(restrict) = %v, want %v", got, want)
	}

	all := ParseAvailableCodecs(encodersOutput, false)
	for _, name := range []string{"libx264", "aac", "some_unknown_codec"} {
		if !all[name] {
			t.Errorf("ParseAvailableCodecs(all) missing %q", name)
		}
	}
	if all["Video"] {
		t.Error("ParseAvailableCodecs picked up the legend line as an encoder")
	}
}

func TestPresetTranslators(t *testing.T) {
	t.Parallel()

	nvencCases := map[string]string{
		"ultrafast": "p1", "superfast": "p1", "veryfast": "p1",
		"faster": "p2", "fast": "p3", "medium": "p4",
		"slow": "p5", "slower": "p6", "slowest": "p7", "placebo": "p7",
	}
	for preset, want := range nvencCases {
		got, err := NVENCPreset(preset)
		if err != nil {
			t.Fatalf("NVENCPreset(%q): %v", preset, err)
		}
		if got != want {
			t.Errorf("NVENCPreset(%q) = %q, want %q", preset, got, want)
		}
	}
	if _, err := NVENCPreset("turbo"); err == nil {
		t.Error("NVENCPreset should reject an unknown preset")
	}

	amfCases := map[string]string{
		"placebo": "quality", "slowest": "quality", "slower": "quality", "slow": "quality",
		"medium": "balanced",
		"fast":   "speed", "faster": "speed", "veryfast": "speed",
		"superfast": "speed", "ultrafast": "speed",
	}
	for preset, want := range amfCases {
		got, err := MapPresetToAMFQuality(preset)
		if err != nil {
			t.Fatalf("MapPresetToAMFQuality(%q): %v", preset, err)
		}
		if got != want {
			t.Errorf("MapPresetToAMFQuality(%q) = %q, want %q", preset, got, want)
		}
	}

	if _, err := PresetToQSV("ultrafast"); err == nil {
		t.Error("PresetToQSV should reject a preset outside the QSV vocabulary")
	}

	proresCases := map[int]string{0: "proxy", 1: "lt", 2: "standard", 3: "hq", 4: "4444", 5: "xq"}
	for tier, want := range proresCases {
		got, err := ProResProfileName(tier)
		if err != nil {
			t.Fatalf("ProResProfileName(%d): %v", tier, err)
		}
		if got != want {
			t.Errorf("ProResProfileName(%d) = %q, want %q", tier, got, want)
		}
	}
	if _, err := ProResProfileName(6); err == nil {
		t.Error("ProResProfileName should reject an out-of-range tier")
	}
}
