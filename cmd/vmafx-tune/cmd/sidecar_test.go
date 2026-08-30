// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/sidecar_test.go — in-package tests for the sidecar
// operator surface.
//
// The payload shapes pinned here are the ones `vmaf-tune sidecar <cmd> --json`
// emits; a drift breaks any script parsing either implementation's output.

package cmd

import (
	"reflect"
	"strings"
	"testing"

	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/predictor"
)

func TestSidecarCommandTree(t *testing.T) {
	t.Parallel()

	cmd := newSidecarCmd()
	got := map[string]bool{}
	for _, c := range cmd.Commands() {
		got[c.Name()] = true
	}
	for _, want := range []string{"status", "predict", "record", "batch-record"} {
		if !got[want] {
			t.Errorf("sidecar is missing the %q subcommand", want)
		}
	}
}

func TestSidecarRequiredFlags(t *testing.T) {
	t.Parallel()

	tests := []struct {
		sub  string
		want []string
	}{
		{sub: "status"},
		{sub: "predict", want: []string{"features-json", "crf"}},
		{sub: "record", want: []string{"features-json", "crf", "observed-vmaf"}},
		{sub: "batch-record", want: []string{"captures-jsonl"}},
	}

	parent := newSidecarCmd()
	for _, tc := range tests {
		t.Run(tc.sub, func(t *testing.T) {
			t.Parallel()
			child := findChild(parent, tc.sub)
			if child == nil {
				t.Fatalf("no %q subcommand", tc.sub)
			}
			for _, name := range tc.want {
				flag := child.Flags().Lookup(name)
				if flag == nil {
					t.Fatalf("%s is missing the --%s flag", tc.sub, name)
				}
				if flag.Annotations["cobra_annotation_bash_completion_one_required_flag"] == nil {
					t.Errorf("%s --%s should be required", tc.sub, name)
				}
			}
			// Every child carries the shared configuration flags.
			for _, name := range []string{"codec", "cache-dir", "predictor-version", "model", "json"} {
				if child.Flags().Lookup(name) == nil {
					t.Errorf("%s is missing the shared --%s flag", tc.sub, name)
				}
			}
		})
	}
}

func TestShotFeaturesFromMapping(t *testing.T) {
	t.Parallel()

	full := map[string]any{
		"probe_bitrate_kbps":      4200.5,
		"probe_i_frame_avg_bytes": 51234.0,
		"probe_p_frame_avg_bytes": 8123.25,
		"probe_b_frame_avg_bytes": 2011.75,
		"saliency_mean":           0.42,
		"saliency_var":            0.031,
		"frame_diff_mean":         7.5,
		"y_avg":                   112.25,
		"y_var":                   1830.5,
		"shot_length_frames":      240.0,
		"fps":                     24.0,
		"width":                   1920.0,
		"height":                  1080.0,
	}
	wantFull := predictor.ShotFeatures{
		ProbeBitrateKbps: 4200.5, ProbeIFrameAvgBytes: 51234.0,
		ProbePFrameAvgBytes: 8123.25, ProbeBFrameAvgBytes: 2011.75,
		SaliencyMean: 0.42, SaliencyVar: 0.031, FrameDiffMean: 7.5,
		YAvg: 112.25, YVar: 1830.5, ShotLengthFrames: 240,
		FPS: 24.0, Width: 1920, Height: 1080,
	}

	minimal := map[string]any{
		"probe_bitrate_kbps":      1000.0,
		"probe_i_frame_avg_bytes": 0.0,
		"probe_p_frame_avg_bytes": 0.0,
		"probe_b_frame_avg_bytes": 0.0,
	}

	tests := []struct {
		name    string
		row     map[string]any
		want    predictor.ShotFeatures
		wantErr string
	}{
		{name: "every field present", row: full, want: wantFull},
		{
			name: "optional fields default to zero",
			row:  minimal,
			want: predictor.ShotFeatures{ProbeBitrateKbps: 1000.0},
		},
		{
			name: "a features wrapper is unwrapped",
			row:  map[string]any{"features": minimal, "crf": 26.0},
			want: predictor.ShotFeatures{ProbeBitrateKbps: 1000.0},
		},
		{
			// The four probe_* fields have no meaningful default: a zero
			// probe bitrate would train the fit on a fabricated
			// complexity barometer.
			name: "a missing required key is rejected",
			row:  map[string]any{"probe_bitrate_kbps": 1000.0},
			wantErr: "features missing required keys: probe_i_frame_avg_bytes, " +
				"probe_p_frame_avg_bytes, probe_b_frame_avg_bytes",
		},
		{
			name:    "a non-object features wrapper is rejected",
			row:     map[string]any{"features": "not an object"},
			wantErr: "'features' must be a JSON object",
		},
		{
			name: "a non-numeric feature value is rejected rather than zeroed",
			row: map[string]any{
				"probe_bitrate_kbps": "fast", "probe_i_frame_avg_bytes": 0.0,
				"probe_p_frame_avg_bytes": 0.0, "probe_b_frame_avg_bytes": 0.0,
			},
			wantErr: "invalid sidecar feature value",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := shotFeaturesFromMapping(tc.row)
			if tc.wantErr != "" {
				if err == nil {
					t.Fatalf("shotFeaturesFromMapping accepted %v", tc.row)
				}
				if !strings.Contains(err.Error(), tc.wantErr) {
					t.Errorf("error = %q, want it to contain %q", err, tc.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("shotFeaturesFromMapping: %v", err)
			}
			if got != tc.want {
				t.Errorf("shotFeaturesFromMapping() = %+v, want %+v", got, tc.want)
			}
		})
	}
}

func TestParseCaptureRow(t *testing.T) {
	t.Parallel()

	const valid = `{"probe_bitrate_kbps": 4200.5, "probe_i_frame_avg_bytes": 0,
		"probe_p_frame_avg_bytes": 0, "probe_b_frame_avg_bytes": 0,
		"crf": 26, "observed_vmaf": 91.75}`

	tests := []struct {
		name         string
		line         string
		wantCRF      int
		wantObserved float64
		wantErr      string
	}{
		{name: "a complete row", line: valid, wantCRF: 26, wantObserved: 91.75},
		{
			// The Python handler surfaces a bare KeyError repr here; the
			// message is what an operator sees on the skip line.
			name: "a missing crf",
			line: `{"probe_bitrate_kbps": 1, "probe_i_frame_avg_bytes": 0,
				"probe_p_frame_avg_bytes": 0, "probe_b_frame_avg_bytes": 0,
				"observed_vmaf": 91.75}`,
			wantErr: "'crf'",
		},
		{
			name: "a missing observed_vmaf",
			line: `{"probe_bitrate_kbps": 1, "probe_i_frame_avg_bytes": 0,
				"probe_p_frame_avg_bytes": 0, "probe_b_frame_avg_bytes": 0,
				"crf": 26}`,
			wantErr: "'observed_vmaf'",
		},
		{name: "unparseable JSON", line: "not json", wantErr: "invalid character"},
		{
			name:    "missing feature keys",
			line:    `{"crf": 26, "observed_vmaf": 91.75}`,
			wantErr: "features missing required keys",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, crf, observed, err := parseCaptureRow(tc.line)
			if tc.wantErr != "" {
				if err == nil {
					t.Fatalf("parseCaptureRow accepted %q", tc.line)
				}
				if !strings.Contains(err.Error(), tc.wantErr) {
					t.Errorf("error = %q, want it to contain %q", err, tc.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("parseCaptureRow: %v", err)
			}
			if crf != tc.wantCRF {
				t.Errorf("crf = %d, want %d", crf, tc.wantCRF)
			}
			if observed != tc.wantObserved {
				t.Errorf("observed_vmaf = %v, want %v", observed, tc.wantObserved)
			}
		})
	}
}

func TestNumericField(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		in     any
		want   float64
		wantOK bool
	}{
		{name: "a JSON number", in: 26.5, want: 26.5, wantOK: true},
		{name: "a quoted number", in: "26.5", want: 26.5, wantOK: true},
		{name: "a quoted number with whitespace", in: "  26.5 ", want: 26.5, wantOK: true},
		// Python's float(True) is 1.0, so a JSON boolean is accepted.
		{name: "true is 1.0", in: true, want: 1.0, wantOK: true},
		{name: "false is 0.0", in: false, want: 0.0, wantOK: true},
		{name: "a non-numeric string is rejected", in: "fast"},
		{name: "nil is rejected", in: nil},
		{name: "an object is rejected", in: map[string]any{}},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, ok := numericField(tc.in)
			if ok != tc.wantOK {
				t.Fatalf("numericField(%v) ok = %v, want %v", tc.in, ok, tc.wantOK)
			}
			if ok && got != tc.want {
				t.Errorf("numericField(%v) = %v, want %v", tc.in, got, tc.want)
			}
		})
	}
}

func TestNewBasePredictorRejectsAnONNXModel(t *testing.T) {
	t.Parallel()

	// The Go sidecar has no in-process ONNX runtime; scoring against the
	// analytical curve while the operator asked for a learned model would
	// be a wrong-but-plausible number, so the flag is refused outright.
	if _, err := newBasePredictor("/models/predictor_libx264.onnx"); err == nil {
		t.Fatal("newBasePredictor accepted a --model path")
	} else if !strings.Contains(err.Error(), "ONNX") {
		t.Errorf("error = %q, want it to explain the ONNX gap", err)
	}

	p, err := newBasePredictor("")
	if err != nil {
		t.Fatalf("newBasePredictor(\"\"): %v", err)
	}
	if p == nil {
		t.Fatal("newBasePredictor returned a nil predictor")
	}
}

func TestSidecarStatusPayloadShape(t *testing.T) {
	t.Parallel()

	flags := &sidecarFlags{
		codec:            "libx264",
		cacheDir:         t.TempDir(),
		predictorVersion: "predictor_v1",
	}
	sp, err := buildSidecarPredictor(flags)
	if err != nil {
		t.Fatalf("buildSidecarPredictor: %v", err)
	}
	payload := sidecarStatusPayload(sp)

	wantKeys := []string{
		"schema", "codec", "host_uuid", "state_path",
		"predictor_version", "schema_version", "n_updates", "recent_residual_rms",
	}
	for _, key := range wantKeys {
		if _, ok := payload[key]; !ok {
			t.Errorf("status payload is missing %q", key)
		}
	}
	if len(payload) != len(wantKeys) {
		t.Errorf("status payload has %d keys, want %d", len(payload), len(wantKeys))
	}
	if payload["schema"] != "vmaf-tune-sidecar-status/v1" {
		t.Errorf("schema = %v, want vmaf-tune-sidecar-status/v1", payload["schema"])
	}
	if payload["codec"] != "libx264" {
		t.Errorf("codec = %v, want libx264", payload["codec"])
	}
	if payload["n_updates"] != 0 {
		t.Errorf("cold-start n_updates = %v, want 0", payload["n_updates"])
	}
	if payload["recent_residual_rms"] != 0.0 {
		t.Errorf("cold-start recent_residual_rms = %v, want 0.0", payload["recent_residual_rms"])
	}
	if !strings.HasSuffix(payload["state_path"].(string), "predictor_v1/libx264/state.json") {
		t.Errorf("state_path = %v, want the per-codec cache layout", payload["state_path"])
	}
}

func TestBuildSidecarPredictorRejectsAnUnknownCodec(t *testing.T) {
	t.Parallel()

	flags := &sidecarFlags{codec: "libx999", cacheDir: t.TempDir()}
	if _, err := buildSidecarPredictor(flags); err == nil {
		t.Fatal("buildSidecarPredictor accepted an unknown codec")
	}
}

func TestSidecarKnownCodecsIsSortedAndNonEmpty(t *testing.T) {
	t.Parallel()

	got := sidecarKnownCodecs()
	if got == "" {
		t.Fatal("sidecarKnownCodecs is empty")
	}
	names := strings.Split(got, ", ")
	sorted := append([]string{}, names...)
	for i := 1; i < len(sorted); i++ {
		if sorted[i-1] > sorted[i] {
			t.Fatalf("codec list is not sorted: %v", names)
		}
	}
	if !reflect.DeepEqual(names[0], "av1_amf") {
		t.Errorf("first codec = %q, want av1_amf", names[0])
	}
}

// findChild returns the named subcommand of parent, or nil.
func findChild(parent *cobra.Command, name string) *cobra.Command {
	for _, c := range parent.Commands() {
		if c.Name() == name {
			return c
		}
	}
	return nil
}
