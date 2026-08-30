// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Tests for the plan JSON emitter's byte-compatibility with Python's
// json.dumps(..., indent=2, sort_keys=True).
//
// The golden document in TestRenderPlanJSON_GoldenMatchesPython was produced
// by running tools/vmaf-tune/src/vmaftune/per_shot.py merge_shots + the
// plan_doc block from cli._run_tune_per_shot over the same inputs, and
// pasting its stdout verbatim.

package pershot

import (
	"math"
	"strings"
	"testing"
)

func TestFormatPyFloat(t *testing.T) {
	t.Parallel()
	// Expectations are Python repr() outputs, which is what json.dumps emits.
	cases := []struct {
		in   float64
		want string
	}{
		{24.0, "24.0"},
		{92.0, "92.0"},
		{0, "0.0"},
		{-0.0, "0.0"},
		{1234.57, "1234.57"},
		{92.5, "92.5"},
		{93.25, "93.25"},
		{0.1, "0.1"},
		{-15.5, "-15.5"},
		{1e-4, "0.0001"},
		{1e-5, "1e-05"},
		{1e15, "1000000000000000.0"},
		{1e16, "1e+16"},
		{1e21, "1e+21"},
	}
	for _, tc := range cases {
		if got := formatPyFloat(tc.in); got != tc.want {
			t.Errorf("formatPyFloat(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestPyFloat_RejectsNonFinite(t *testing.T) {
	t.Parallel()
	for _, v := range []float64{math.NaN(), math.Inf(1), math.Inf(-1)} {
		if _, err := pyFloat(v).MarshalJSON(); err == nil {
			t.Errorf("pyFloat(%v).MarshalJSON() should error; JSON has no such literal", v)
		}
	}
}

func TestShotBitrate(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name   string
		in     float64
		wantOK bool
		want   float64
	}{
		{"rounds to two decimals", 1234.5678, true, 1234.57},
		{"already exact", 900.0, true, 900.0},
		// Python's round() breaks exact ties to even; 1234.565 is stored just
		// below the tie, so both implementations land on 1234.56.
		{"tie-ish value", 1234.565, true, 1234.56},
		{"NaN becomes null", math.NaN(), false, 0},
		{"Inf becomes null", math.Inf(1), false, 0},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := shotBitrate(tc.in)
			if !tc.wantOK {
				if got != nil {
					t.Errorf("shotBitrate(%v) = %v, want nil", tc.in, *got)
				}
				return
			}
			if got == nil {
				t.Fatalf("shotBitrate(%v) = nil, want %v", tc.in, tc.want)
			}
			if math.Abs(float64(*got)-tc.want) > 1e-9 {
				t.Errorf("shotBitrate(%v) = %v, want %v", tc.in, *got, tc.want)
			}
		})
	}
}

func TestEnsureASCII(t *testing.T) {
	t.Parallel()
	// Expectations are Python json.dumps outputs (ensure_ascii=True default).
	// The escape sequences are assembled from a literal backslash so the
	// source file stays pure ASCII on the "want" side.
	bs := `\`
	cases := []struct{ in, want string }{
		{`{"a": "plain"}`, `{"a": "plain"}`},
		{"{\"a\": \"café\"}", `{"a": "caf` + bs + `u00e9"}`},
		// Astral-plane runes become a UTF-16 surrogate pair, as CPython emits.
		{"\"\U0001F3AC\"", `"` + bs + `ud83c` + bs + `udfac"`},
	}
	for _, tc := range cases {
		if got := ensureASCII(tc.in); got != tc.want {
			t.Errorf("ensureASCII(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

// goldenPlanJSON is the verbatim stdout of the Python emitter for the plan
// built in TestRenderPlanJSON_GoldenMatchesPython.
const goldenPlanJSON = `{
  "concat_command": [
    "ffmpeg",
    "-y",
    "-hide_banner",
    "-f",
    "concat",
    "-safe",
    "0",
    "-i",
    "segments/concat.txt",
    "-c",
    "copy",
    "per_shot_encode.mp4"
  ],
  "encoder": "libx264",
  "framerate": 24.0,
  "predicate": "bisect",
  "segment_commands": [
    [
      "ffmpeg",
      "-y",
      "-hide_banner",
      "-ss",
      "0.000000",
      "-i",
      "src.mp4",
      "-frames:v",
      "48",
      "-c:v",
      "libx264",
      "-preset",
      "medium",
      "-crf",
      "22",
      "segments/shot_0000.mp4"
    ],
    [
      "ffmpeg",
      "-y",
      "-hide_banner",
      "-ss",
      "2.000000",
      "-i",
      "src.mp4",
      "-frames:v",
      "48",
      "-c:v",
      "libx264",
      "-preset",
      "medium",
      "-crf",
      "27",
      "segments/shot_0001.mp4"
    ]
  ],
  "shots": [
    {
      "bitrate_kbps": 1234.57,
      "crf": 22,
      "end_frame": 48,
      "predicted_vmaf": 92.5,
      "start_frame": 0
    },
    {
      "bitrate_kbps": null,
      "crf": 27,
      "end_frame": 96,
      "predicted_vmaf": 91.0,
      "start_frame": 48
    }
  ],
  "target_vmaf": 92.0
}`

func TestRenderPlanJSON_GoldenMatchesPython(t *testing.T) {
	t.Parallel()
	recs := []Recommendation{
		{Shot: Shot{0, 48}, CRF: 22, PredictedVMAF: 92.5, BitratekBps: 1234.5678},
		{Shot: Shot{48, 96}, CRF: 27, PredictedVMAF: 91.0, BitratekBps: math.NaN()},
	}
	plan, err := Merge(recs, MergeParams{
		Source:    "src.mp4",
		Output:    "per_shot_encode.mp4",
		Framerate: 24.0,
		Encoder:   "libx264",
		FFmpegBin: "ffmpeg",
	})
	if err != nil {
		t.Fatalf("Merge: %v", err)
	}
	got, err := RenderPlanJSON(plan, "bisect", 92.0)
	if err != nil {
		t.Fatalf("RenderPlanJSON: %v", err)
	}
	if got != goldenPlanJSON {
		t.Errorf("plan JSON diverged from the Python golden.\n--- got ---\n%s\n--- want ---\n%s",
			got, goldenPlanJSON)
	}
	// Key order is what sort_keys=True guarantees; assert it explicitly so a
	// future struct-field reshuffle fails loudly rather than silently.
	wantOrder := []string{
		`"concat_command"`, `"encoder"`, `"framerate"`, `"predicate"`,
		`"segment_commands"`, `"shots"`, `"target_vmaf"`,
	}
	pos := -1
	for _, key := range wantOrder {
		idx := strings.Index(got, key)
		if idx <= pos {
			t.Errorf("top-level key %s is out of alphabetical order", key)
		}
		pos = idx
	}
}

func TestRenderPlanJSON_EmptyPlanStillEmitsArrays(t *testing.T) {
	t.Parallel()
	// A hand-built plan with no segments must emit "[]" rather than "null":
	// Python's list comprehension always yields a list, and a null would
	// break any consumer iterating the field.
	got, err := RenderPlanJSON(EncodingPlan{
		Encoder:       "libx264",
		Framerate:     24,
		ConcatCommand: []string{"ffmpeg"},
	}, "bisect", 92)
	if err != nil {
		t.Fatalf("RenderPlanJSON: %v", err)
	}
	for _, want := range []string{`"segment_commands": []`, `"shots": []`} {
		if !strings.Contains(got, want) {
			t.Errorf("output missing %q; got:\n%s", want, got)
		}
	}
}

func TestRenderPlanJSON_RejectsNonFinitePredictedVMAF(t *testing.T) {
	t.Parallel()
	// predicted_vmaf has no null mapping in the Python schema, so a
	// non-finite value must surface as an error rather than a bare NaN token
	// that no RFC 8259 parser accepts (cmd/vmafx-tune/AGENTS.md #2).
	plan, err := Merge(
		[]Recommendation{{Shot: Shot{0, 24}, CRF: 22, PredictedVMAF: math.NaN()}},
		MergeParams{Source: "s.mp4", Output: "o.mp4", Framerate: 24},
	)
	if err != nil {
		t.Fatalf("Merge: %v", err)
	}
	if _, err := RenderPlanJSON(plan, "bisect", 92); err == nil {
		t.Error("RenderPlanJSON should reject a non-finite predicted_vmaf")
	}
}
