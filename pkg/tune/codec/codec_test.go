// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package codec

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

// pythonAdapter mirrors one entry of testdata/python_adapters.json, dumped
// from vmaftune.codec_adapters.
type pythonAdapter struct {
	Name            string   `json:"name"`
	Encoder         string   `json:"encoder"`
	QualityKnob     string   `json:"quality_knob"`
	QualityLo       int      `json:"quality_lo"`
	QualityHi       int      `json:"quality_hi"`
	QualityDefault  int      `json:"quality_default"`
	InvertQuality   bool     `json:"invert_quality"`
	ProbePreset     string   `json:"probe_preset"`
	ProbeQuality    int      `json:"probe_quality"`
	SupportsTwoPass bool     `json:"supports_two_pass"`
	Presets         []string `json:"presets"`
	Argv            map[string]struct {
		CodecArgs []string `json:"codec_args"`
		Extra     []string `json:"extra"`
	} `json:"argv"`
}

func loadPythonAdapters(t *testing.T) []pythonAdapter {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("testdata", "python_adapters.json"))
	if err != nil {
		t.Fatalf("read adapter fixture: %v", err)
	}
	var adapters []pythonAdapter
	if err := json.Unmarshal(raw, &adapters); err != nil {
		t.Fatalf("parse adapter fixture: %v", err)
	}
	if len(adapters) == 0 {
		t.Fatal("adapter fixture is empty")
	}
	return adapters
}

// TestRegistryMatchesPythonMetadata is the parity gate on the adapter table:
// every field the auto planner reads (the quality window, the default, the
// probe knobs, the two-pass flag, the preset vocabulary) must match the
// Python registry entry it was transcribed from. A drift here silently
// changes which CRF the planner picks.
func TestRegistryMatchesPythonMetadata(t *testing.T) {
	t.Parallel()

	for _, want := range loadPythonAdapters(t) {
		t.Run(want.Name, func(t *testing.T) {
			t.Parallel()
			got, err := Get(want.Name)
			if err != nil {
				t.Fatalf("Get(%q): %v", want.Name, err)
			}
			checks := []struct {
				field    string
				got, exp any
			}{
				{"Encoder", got.Encoder, want.Encoder},
				{"QualityKnob", got.QualityKnob, want.QualityKnob},
				{"QualityLo", got.QualityLo, want.QualityLo},
				{"QualityHi", got.QualityHi, want.QualityHi},
				{"QualityDefault", got.QualityDefault, want.QualityDefault},
				{"InvertQuality", got.InvertQuality, want.InvertQuality},
				{"ProbePreset", got.ProbePreset, want.ProbePreset},
				{"ProbeQuality", got.ProbeQuality, want.ProbeQuality},
				{"SupportsTwoPass", got.SupportsTwoPass, want.SupportsTwoPass},
			}
			for _, c := range checks {
				if c.got != c.exp {
					t.Errorf("%s = %v, want %v", c.field, c.got, c.exp)
				}
			}
			if !reflect.DeepEqual(got.Presets, want.Presets) {
				t.Errorf("Presets = %v, want %v", got.Presets, want.Presets)
			}
		})
	}
}

// TestFFmpegCodecArgsMatchPython replays every (adapter, preset) pair through
// both the codec-args and extra-params surfaces and demands identical argv.
// This is what makes a Go-driven encode byte-comparable with a Python-driven
// one — including the AMF family's duplicated rate-control tail, which the
// Python adapters emit from both surfaces and this port reproduces.
func TestFFmpegCodecArgsMatchPython(t *testing.T) {
	t.Parallel()

	for _, want := range loadPythonAdapters(t) {
		t.Run(want.Name, func(t *testing.T) {
			t.Parallel()
			adapter, err := Get(want.Name)
			if err != nil {
				t.Fatalf("Get(%q): %v", want.Name, err)
			}
			for _, preset := range want.Presets {
				expected := want.Argv[preset]
				gotArgs, argsErr := adapter.FFmpegCodecArgs(preset, want.QualityDefault)

				if expected.CodecArgs == nil {
					// The Python adapter raised: this codec has no argv
					// mapping (av1_videotoolbox, ADR-0339).
					var unavailable *ErrUnavailable
					if !errors.As(argsErr, &unavailable) {
						t.Errorf("preset %q: expected an ErrUnavailable, got args=%v err=%v",
							preset, gotArgs, argsErr)
					}
					continue
				}
				if argsErr != nil {
					t.Errorf("preset %q: unexpected error: %v", preset, argsErr)
					continue
				}
				if !reflect.DeepEqual(gotArgs, expected.CodecArgs) {
					t.Errorf("preset %q codec args:\n got %v\nwant %v",
						preset, gotArgs, expected.CodecArgs)
				}
				gotExtra := adapter.ExtraParams(preset, want.QualityDefault)
				if len(gotExtra) == 0 && len(expected.Extra) == 0 {
					continue
				}
				if !reflect.DeepEqual(gotExtra, expected.Extra) {
					t.Errorf("preset %q extra params:\n got %v\nwant %v",
						preset, gotExtra, expected.Extra)
				}
			}
		})
	}
}

// TestKnownMatchesPython pins the registry membership so a codec cannot be
// dropped or added on one side only.
func TestKnownMatchesPython(t *testing.T) {
	t.Parallel()

	want := make([]string, 0, 19)
	for _, a := range loadPythonAdapters(t) {
		want = append(want, a.Name)
	}
	got := Known()
	if !reflect.DeepEqual(got, want) {
		t.Errorf("Known() =\n %v\nwant\n %v", got, want)
	}
}

func TestGetUnknownCodec(t *testing.T) {
	t.Parallel()

	_, err := Get("libnope")
	if err == nil {
		t.Fatal("expected an error for an unknown codec")
	}
	if !strings.Contains(err.Error(), "libnope") {
		t.Errorf("error should name the codec, got %v", err)
	}
}

// TestValidate pins the strict gate, which is separate from the lenient argv
// builder: an out-of-vocabulary preset or an out-of-window quality is an
// error here even though FFmpegCodecArgs tolerates both.
func TestValidate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		codec   string
		preset  string
		quality int
		wantErr string
	}{
		{"valid", "libx264", "medium", 23, ""},
		{"lower bound is inclusive", "libx264", "medium", 0, ""},
		{"upper bound is inclusive", "libx264", "medium", 51, ""},
		{"unknown preset", "libx264", "turbo", 23, "preset"},
		{"quality above the window", "libx264", "medium", 99, "crf"},
		{"quality below the window", "libx265", "medium", 2, "crf"},
		{"nvenc knob is named in the error", "h264_nvenc", "medium", 99, "cq"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			adapter, err := Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			err = adapter.Validate(tc.preset, tc.quality)
			if tc.wantErr == "" {
				if err != nil {
					t.Errorf("unexpected error: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatalf("expected an error mentioning %q", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error %q should mention %q", err, tc.wantErr)
			}
		})
	}
}

// TestFFmpegCodecArgsIsLenient documents the deliberate split from Validate:
// an unknown preset falls back to the adapter's documented default rather
// than failing the encode, matching the Python adapters.
func TestFFmpegCodecArgsIsLenient(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		codec string
		want  []string
	}{
		{
			name: "x264 passes an unknown preset through verbatim", codec: "libx264",
			want: []string{"-c:v", "libx264", "-preset", "turbo", "-crf", "23"},
		},
		{
			name: "nvenc falls back to p4", codec: "h264_nvenc",
			want: []string{"-c:v", "h264_nvenc", "-preset", "p4", "-cq", "23"},
		},
		{
			name: "svt-av1 falls back to preset 7", codec: "libsvtav1",
			want: []string{"-c:v", "libsvtav1", "-preset", "7", "-crf", "35"},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			adapter, err := Get(tc.codec)
			if err != nil {
				t.Fatalf("Get(%q): %v", tc.codec, err)
			}
			got, err := adapter.FFmpegCodecArgs("turbo", adapter.QualityDefault)
			if err != nil {
				t.Fatalf("FFmpegCodecArgs: %v", err)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("args = %v, want %v", got, tc.want)
			}
		})
	}
}
