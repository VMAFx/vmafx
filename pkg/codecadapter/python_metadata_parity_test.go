// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/codecadapter/python_metadata_parity_test.go — the registry-metadata
// parity gate, replayed from testdata/python_adapters.json.
//
// The fixture was dumped from vmaftune.codec_adapters: every field the auto
// planner reads (the quality window, the default, the probe knobs, the
// two-pass flag, the preset vocabulary) plus the argv both adapter surfaces
// emit at the default quality for every preset. It came across from the
// pkg/tune/codec registry when that duplicate was folded into this package
// (ADR-1137), so the metadata contract the planner depends on stays pinned.

package codecadapter

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// pythonAdapter mirrors one entry of testdata/python_adapters.json.
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

// TestRegistryMatchesPythonMetadata is the parity gate on the adapter table.
// A drift here silently changes which CRF the auto planner picks.
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
				{"QualityRange[0]", got.QualityRange[0], want.QualityLo},
				{"QualityRange[1]", got.QualityRange[1], want.QualityHi},
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

// TestCodecArgsMatchPythonAtDefaultQuality replays every (adapter, preset)
// pair through both argv surfaces at the adapter's default quality.
//
// FFmpegCodecArgs must match the Python ffmpeg_codec_args exactly. ExtraParams
// must match the Python extra_params with one documented exception: the AMF
// trio's extra_params returns the same constant-QP block ffmpeg_codec_args
// already emitted, and this registry deliberately drops that inert duplicate
// (AGENTS.md invariant 3, ADR-1125). The test pins the exception precisely —
// the fixture's extra tail must equal the codec-args tail, so any change to
// what the Python duplicates is still caught.
func TestCodecArgsMatchPythonAtDefaultQuality(t *testing.T) {
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
					// The Python adapter raised: the codec has no argv mapping
					// yet (av1_videotoolbox, ADR-0339). The Go placeholder
					// probes the host lazily, so it may legitimately answer on
					// a machine whose FFmpeg has grown the encoder.
					if argsErr == nil {
						t.Skipf("preset %q: host FFmpeg provides %s", preset, want.Name)
					}
					if !errors.Is(argsErr, ErrAv1VideoToolboxUnavailable) {
						t.Errorf("preset %q: want ErrAv1VideoToolboxUnavailable, got %v",
							preset, argsErr)
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

				gotExtra := adapter.ExtraParams()
				if adapter.qualityStyle == StyleAMFQP {
					// The documented deviation: Python's extra is the codec
					// tail repeated; the Go registry emits it once.
					if !reflect.DeepEqual(expected.Extra, expected.CodecArgs[2:]) {
						t.Errorf("preset %q: the AMF fixture no longer duplicates the codec tail "+
							"(extra %v vs codec args %v); revisit the de-duplication",
							preset, expected.Extra, expected.CodecArgs)
					}
					if len(gotExtra) != 0 {
						t.Errorf("preset %q: AMF extra params = %v, want none", preset, gotExtra)
					}
					continue
				}
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

// TestKnownMatchesPythonRegistry pins the registry membership and its sorted
// order, so a codec cannot be dropped or added on one side only.
func TestKnownMatchesPythonRegistry(t *testing.T) {
	t.Parallel()

	adapters := loadPythonAdapters(t)
	want := make([]string, 0, len(adapters))
	for _, a := range adapters {
		want = append(want, a.Name)
	}
	if got := Known(); !reflect.DeepEqual(got, want) {
		t.Errorf("Known() =\n %v\nwant\n %v", got, want)
	}
}
