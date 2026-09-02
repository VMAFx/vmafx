// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/marshal_test.go — container layout, struct handling and the
// non-finite policy.
//
// Every `want` string here was produced by running the equivalent value
// through CPython 3's json.dumps with the stated arguments and pasting the
// output verbatim. The cases are the union of what the four
// pre-consolidation encoders pinned independently (ADR-1137), so a regression
// against any one of their contracts fails here.

package pyjson

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
)

// negZero returns -0.0 without tripping the compiler's constant folding.
func negZero() float64 {
	z := 0.0
	return -z
}

// TestMarshalMatchesPythonJSONDumps pins the encoder against real json.dumps
// output across the compact and indented forms.
func TestMarshalMatchesPythonJSONDumps(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   any
		opts Options
		want string
	}{
		{
			name: "nested value with indent=2 and sort_keys",
			in: map[string]any{
				"a": 1, "b": []any{1, 2}, "c": 93.0,
				"d": map[string]any{"e": nil},
			},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"c\": 93.0," +
				"\n  \"d\": {\n    \"e\": null\n  }\n}",
		},
		{
			name: "non-finite floats use Python's tokens",
			in:   map[string]any{"x": math.NaN(), "y": math.Inf(1), "z": -0.5},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"x\": NaN,\n  \"y\": Infinity,\n  \"z\": -0.5\n}",
		},
		{
			name: "empty containers stay on one line",
			in:   map[string]any{"empty_obj": map[string]any{}, "empty_arr": []any{}},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"empty_arr\": [],\n  \"empty_obj\": {}\n}",
		},
		{
			name: "strings escape like ensure_ascii=True",
			in:   map[string]any{"s": `a"b`, "u": "é"},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"s\": \"a\\\"b\",\n  \"u\": \"\\u00e9\"\n}",
		},
		{
			name: "compact mode uses the default separators",
			in:   map[string]any{"b": 1, "a": 2},
			opts: Options{SortKeys: true},
			want: `{"a": 2, "b": 1}`,
		},
		{
			name: "arrays of objects nest correctly",
			in: map[string]any{
				"nested": []any{map[string]any{"k": 1.0}, map[string]any{"k": 2.5}},
			},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"nested\": [\n    {\n      \"k\": 1.0\n    },\n" +
				"    {\n      \"k\": 2.5\n    }\n  ]\n}",
		},
		{"empty object", map[string]any{}, Options{Indent: "  "}, "{}"},
		{"empty array", []any{}, Options{Indent: "  "}, "[]"},
		{"null", nil, Options{Indent: "  "}, "null"},
		{"bool", true, Options{Indent: "  "}, "true"},
		{"int", 7, Options{Indent: "  "}, "7"},
		{"int64", int64(9007199254740993), Options{}, "9007199254740993"},
		{"float gets dot zero", 92.0, Options{}, "92.0"},
		{"string", "hi", Options{}, `"hi"`},
		{
			name: "keys are sorted by code point",
			in:   map[string]any{"b": 1, "a": 2, "C": 3},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"C\": 3,\n  \"a\": 2,\n  \"b\": 1\n}",
		},
		{
			name: "scalar list",
			in:   []any{1, 2.5, "three", nil, true},
			opts: Options{Indent: "  "},
			want: "[\n  1,\n  2.5,\n  \"three\",\n  null,\n  true\n]",
		},
		{
			name: "nested indentation",
			in:   map[string]any{"outer": map[string]any{"inner": []any{1, 2}}},
			opts: Options{SortKeys: true, Indent: "  "},
			want: "{\n  \"outer\": {\n    \"inner\": [\n      1,\n      2\n    ]\n  }\n}",
		},
		{
			name: "compact form uses a comma-space separator",
			in:   map[string]any{"a": 1, "b": []any{2, 3}},
			opts: Options{SortKeys: true},
			want: `{"a": 1, "b": [2, 3]}`,
		},
		{
			name: "float slices keep repr spelling and the NaN token",
			in:   map[string]any{"xs": []any{1.0, 0.5, math.NaN()}},
			opts: Options{SortKeys: true},
			want: `{"xs": [1.0, 0.5, NaN]}`,
		},
		{
			name: "string and typed slices",
			in: map[string]any{
				"extra_params": []string{"-vf", "scale=640:480"},
				"crfs":         []int{23, 28},
				"hdr_forced":   false,
			},
			opts: Options{SortKeys: true},
			want: `{"crfs": [23, 28], "extra_params": ["-vf", "scale=640:480"], "hdr_forced": false}`,
		},
		{
			name: "json.Number integral",
			in:   map[string]any{"crf": json.Number("23")},
			opts: Options{Indent: "  "},
			want: "{\n  \"crf\": 23\n}",
		},
		{
			name: "json.Number fractional",
			in:   map[string]any{"crf": json.Number("23.0")},
			opts: Options{Indent: "  "},
			want: "{\n  \"crf\": 23.0\n}",
		},
		{
			name: "json.Number exponent normalises like CPython",
			in:   map[string]any{"crf": json.Number("2.30e1")},
			opts: Options{Indent: "  "},
			want: "{\n  \"crf\": 23.0\n}",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := Marshal(tc.in, tc.opts)
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			if string(got) != tc.want {
				t.Errorf("Marshal =\n%s\nwant\n%s", got, tc.want)
			}
		})
	}
}

// TestMarshalMatchesCPythonDumpsComposite pins one composite payload against
// the exact bytes CPython's json.dumps(payload, indent=2, sort_keys=True)
// produced for the equivalent dict. It covers key sorting, indentation, empty
// containers, ensure_ascii escaping, HTML metacharacters and the float repr
// in one shot.
func TestMarshalMatchesCPythonDumpsComposite(t *testing.T) {
	t.Parallel()

	payload := map[string]any{
		"zebra":      1,
		"alpha":      []any{1, 2.5, "three", nil, true, false},
		"empty_list": []any{},
		"empty_dict": map[string]any{},
		"nested": map[string]any{
			"b": map[string]any{"c": []any{map[string]any{"d": 92.0}}},
			"a": 1e16,
		},
		"unicode": "café — 日本語 \U0001F600",
		"html":    "<a href='x'>&amp;</a>",
		"ctrl":    "tab\there\nnewline\\backslash\"quote\x01",
		"floats":  []any{0.0, negZero(), 1e-5, 0.0001, 1234567.0, 1.0 / 3.0, -3.25},
		"ints":    []any{0, -1, int64(9007199254740993)},
		"del":     "\x7f",
	}

	want := strings.Join([]string{
		`{`,
		`  "alpha": [`,
		`    1,`,
		`    2.5,`,
		`    "three",`,
		`    null,`,
		`    true,`,
		`    false`,
		`  ],`,
		`  "ctrl": "tab\there\nnewline\\backslash\"quote\u0001",`,
		`  "del": "\u007f",`,
		`  "empty_dict": {},`,
		`  "empty_list": [],`,
		`  "floats": [`,
		`    0.0,`,
		`    -0.0,`,
		`    1e-05,`,
		`    0.0001,`,
		`    1234567.0,`,
		`    0.3333333333333333,`,
		`    -3.25`,
		`  ],`,
		`  "html": "<a href='x'>&amp;</a>",`,
		`  "ints": [`,
		`    0,`,
		`    -1,`,
		`    9007199254740993`,
		`  ],`,
		`  "nested": {`,
		`    "a": 1e+16,`,
		`    "b": {`,
		`      "c": [`,
		`        {`,
		`          "d": 92.0`,
		`        }`,
		`      ]`,
		`    }`,
		`  },`,
		`  "unicode": "caf\u00e9 \u2014 \u65e5\u672c\u8a9e \ud83d\ude00",`,
		`  "zebra": 1`,
		`}`,
	}, "\n")

	got, err := Marshal(payload, Options{SortKeys: true, Indent: "  ", NonFinite: NaNAsNull})
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if string(got) != want {
		t.Errorf("Marshal output differs from CPython json.dumps\n--- got ---\n%s\n--- want ---\n%s",
			got, want)
	}
}

// reportPayload mirrors the shape the subcommands emit: a struct with json
// tags, a nested struct, a pointer field and a slice of structs.
type reportPayload struct {
	Verdict    string       `json:"verdict"`
	TargetVMAF float64      `json:"target_vmaf"`
	Alpha      *float64     `json:"alpha"`
	Rows       []payloadRow `json:"rows"`
	Meta       payloadMeta  `json:"meta"`
	Skipped    string       `json:"-"`
	Optional   string       `json:"optional,omitempty"`
}

type payloadRow struct {
	CRF  int     `json:"crf"`
	VMAF float64 `json:"vmaf"`
	Note *string `json:"note,omitempty"`
}

type payloadMeta struct {
	Enabled bool `json:"enabled"`
}

// TestMarshalStructs covers json-tag handling on the payload shapes the
// subcommands actually emit: renamed fields, "-", omitempty, nil pointers and
// declaration-order versus sorted output.
func TestMarshalStructs(t *testing.T) {
	t.Parallel()

	payload := reportPayload{
		Verdict:    "gospel",
		TargetVMAF: 93.0,
		Rows: []payloadRow{
			{CRF: 23, VMAF: 96.0},
			{CRF: 28, VMAF: 90.5},
		},
		Meta:    payloadMeta{Enabled: true},
		Skipped: "never emitted",
	}

	t.Run("declaration order", func(t *testing.T) {
		t.Parallel()

		got, err := MarshalIndent(payload, false)
		if err != nil {
			t.Fatalf("MarshalIndent: %v", err)
		}
		want := "{\n  \"verdict\": \"gospel\",\n  \"target_vmaf\": 93.0," +
			"\n  \"alpha\": null,\n  \"rows\": [\n    {\n      \"crf\": 23," +
			"\n      \"vmaf\": 96.0\n    },\n    {\n      \"crf\": 28," +
			"\n      \"vmaf\": 90.5\n    }\n  ],\n  \"meta\": {" +
			"\n    \"enabled\": true\n  }\n}"
		if string(got) != want {
			t.Errorf("MarshalIndent =\n%s\nwant\n%s", got, want)
		}
	})

	t.Run("sorted keys", func(t *testing.T) {
		t.Parallel()

		got, err := Marshal(payload, Options{SortKeys: true})
		if err != nil {
			t.Fatalf("Marshal: %v", err)
		}
		want := `{"alpha": null, "meta": {"enabled": true}, ` +
			`"rows": [{"crf": 23, "vmaf": 96.0}, {"crf": 28, "vmaf": 90.5}], ` +
			`"target_vmaf": 93.0, "verdict": "gospel"}`
		if string(got) != want {
			t.Errorf("Marshal =\n%s\nwant\n%s", got, want)
		}
	})

	t.Run("omitempty and pointer fields", func(t *testing.T) {
		t.Parallel()

		note := "recalibrated"
		alpha := 0.05
		withOptional := payload
		withOptional.Optional = "present"
		withOptional.Alpha = &alpha
		withOptional.Rows = []payloadRow{{CRF: 23, VMAF: 96.0, Note: &note}}

		got, err := Marshal(withOptional, Options{SortKeys: true})
		if err != nil {
			t.Fatalf("Marshal: %v", err)
		}
		// SortKeys applies recursively, so the nested row's keys sort too.
		want := `{"alpha": 0.05, "meta": {"enabled": true}, ` +
			`"optional": "present", ` +
			`"rows": [{"crf": 23, "note": "recalibrated", "vmaf": 96.0}], ` +
			`"target_vmaf": 93.0, "verdict": "gospel"}`
		if string(got) != want {
			t.Errorf("Marshal =\n%s\nwant\n%s", got, want)
		}
	})
}

// TestMarshalNilContainers pins the one place the consolidated encoder
// departs from encoding/json: a nil slice or map is the empty Python
// container, not None. Every tree-building consumer (the auto plan, the
// sidecar state, the corpus row) builds lists with `var xs []any` and expects
// "[]" when nothing was appended; "absent" is modelled with a pointer or a
// nil interface, which still render as null.
func TestMarshalNilContainers(t *testing.T) {
	t.Parallel()

	var nilSlice []int
	var nilMap map[string]int
	var nilPtr *int
	var nilAny any
	got, err := Marshal(map[string]any{"s": nilSlice, "m": nilMap, "p": nilPtr, "i": nilAny},
		Options{SortKeys: true})
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if string(got) != `{"i": null, "m": {}, "p": null, "s": []}` {
		t.Errorf("Marshal = %s, want nil containers as [] / {} and nil pointers as null", got)
	}
}

// TestMarshalRejectsUnsupportedTypes checks the deliberate strictness: a
// value with no JSON meaning is an error, not a silent guess, so schema drift
// fails in a test rather than emitting wrong bytes.
func TestMarshalRejectsUnsupportedTypes(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   any
	}{
		{"channel", map[string]any{"x": make(chan int)}},
		{"function", map[string]any{"x": func() {}}},
		{"complex", map[string]any{"x": complex(1, 2)}},
		{"non-string map key", map[int]string{1: "a"}},
		{"uninterpretable json.Number", map[string]any{"x": json.Number("nope")}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if _, err := Marshal(tc.in, Options{}); err == nil {
				t.Fatal("expected an error for an unsupported value, got nil")
			}
		})
	}
	if _, err := MarshalSorted(map[string]any{"x": make(chan int)}); err == nil {
		t.Fatal("MarshalSorted accepted an unsupported value type")
	}
	if _, err := MarshalIndentSorted(map[string]any{"x": make(chan int)}, 2); err == nil {
		t.Fatal("MarshalIndentSorted accepted an unsupported value type")
	}
}

// TestFloatNaNPolicy pins the two non-finite renderings side by side.
func TestFloatNaNPolicy(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		in     float64
		policy NaNPolicy
		want   string
	}{
		{"nan as null", math.NaN(), NaNAsNull, "null"},
		{"pos inf as null", math.Inf(1), NaNAsNull, "null"},
		{"neg inf as null", math.Inf(-1), NaNAsNull, "null"},
		{"nan as token", math.NaN(), NaNAsToken, "NaN"},
		{"pos inf as token", math.Inf(1), NaNAsToken, "Infinity"},
		{"neg inf as token", math.Inf(-1), NaNAsToken, "-Infinity"},
		{"finite ignores policy", 1.5, NaNAsToken, "1.5"},
		{"finite ignores policy under null", 1.5, NaNAsNull, "1.5"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := Marshal(tc.in, Options{NonFinite: tc.policy})
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			if string(got) != tc.want {
				t.Errorf("Marshal(%v, %v) = %q, want %q", tc.in, tc.policy, got, tc.want)
			}
		})
	}
}

// TestMarshalStrict pins the portable RFC-8259 path the sidecar state file
// and the executor's JSONL rows use: sorted keys, non-finite floats as null,
// everything else untouched.
func TestMarshalStrict(t *testing.T) {
	t.Parallel()

	in := map[string]any{
		"finite":    1.5,
		"nan":       math.NaN(),
		"posinf":    math.Inf(1),
		"neginf":    math.Inf(-1),
		"nested":    map[string]any{"deep": math.NaN()},
		"list":      []any{1.0, math.NaN()},
		"untouched": "text",
	}
	got, err := MarshalStrict(in, 0)
	if err != nil {
		t.Fatalf("MarshalStrict: %v", err)
	}
	for _, forbidden := range []string{"NaN", "Infinity"} {
		if strings.Contains(got, forbidden) {
			t.Errorf("strict output still carries %q: %s", forbidden, got)
		}
	}
	want := `{"finite": 1.5, "list": [1.0, null], "nan": null, "neginf": null, ` +
		`"nested": {"deep": null}, "posinf": null, "untouched": "text"}`
	if got != want {
		t.Errorf("MarshalStrict =\n%s\nwant\n%s", got, want)
	}

	indented, err := MarshalStrict(map[string]any{"a": math.NaN(), "b": []any{}}, 2)
	if err != nil {
		t.Fatalf("MarshalStrict: %v", err)
	}
	if wantIndented := "{\n  \"a\": null,\n  \"b\": []\n}"; indented != wantIndented {
		t.Errorf("MarshalStrict(indent=2) =\n%s\nwant\n%s", indented, wantIndented)
	}
}

// TestMarshalIndentSorted pins the indented sort_keys form the sidecar status
// payloads use, against verbatim json.dumps(payload, indent=2,
// sort_keys=True) output.
func TestMarshalIndentSorted(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		obj  map[string]any
		want string
	}{
		{
			name: "sidecar status payload",
			obj: map[string]any{
				"schema":              "vmaf-tune-sidecar-status/v1",
				"codec":               "libx264",
				"host_uuid":           "0123456789abcdef0123456789abcdef",
				"state_path":          "/home/u/.cache/vmaf-tune/sidecar/predictor_v1/libx264/state.json",
				"predictor_version":   "predictor_v1",
				"schema_version":      1,
				"n_updates":           0,
				"recent_residual_rms": 0.0,
			},
			want: `{
  "codec": "libx264",
  "host_uuid": "0123456789abcdef0123456789abcdef",
  "n_updates": 0,
  "predictor_version": "predictor_v1",
  "recent_residual_rms": 0.0,
  "schema": "vmaf-tune-sidecar-status/v1",
  "schema_version": 1,
  "state_path": "/home/u/.cache/vmaf-tune/sidecar/predictor_v1/libx264/state.json"
}`,
		},
		{
			name: "nested containers and empty collections",
			obj: map[string]any{
				"a": map[string]any{
					"b": []any{1, 2.5, "x"},
					"c": map[string]any{},
				},
				"d": []string{},
				"e": true,
			},
			want: `{
  "a": {
    "b": [
      1,
      2.5,
      "x"
    ],
    "c": {}
  },
  "d": [],
  "e": true
}`,
		},
		{name: "empty object", obj: map[string]any{}, want: "{}"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := MarshalIndentSorted(tc.obj, 2)
			if err != nil {
				t.Fatalf("MarshalIndentSorted: %v", err)
			}
			if got != tc.want {
				t.Errorf("MarshalIndentSorted() =\n%s\nwant\n%s", got, tc.want)
			}
		})
	}
}

// TestMarshalSorted pins the compact sort_keys form the corpus JSONL uses.
func TestMarshalSorted(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		row  map[string]any
		want string
	}{
		{
			name: "keys are sorted and floats use repr",
			row:  map[string]any{"zebra": 1, "alpha": "a", "framerate": 24.0},
			want: `{"alpha": "a", "framerate": 24.0, "zebra": 1}`,
		},
		{
			name: "non-finite floats use the bare CPython tokens",
			row:  map[string]any{"adm2_mean": math.NaN(), "pos": math.Inf(1), "neg": math.Inf(-1)},
			want: `{"adm2_mean": NaN, "neg": -Infinity, "pos": Infinity}`,
		},
		{
			name: "empty list renders as []",
			row:  map[string]any{"extra_params": []string{}},
			want: `{"extra_params": []}`,
		},
		{
			name: "non-ascii is escaped like ensure_ascii=True",
			row:  map[string]any{"src": "clip-\u00e9.yuv"},
			want: `{"src": "clip-\u00e9.yuv"}`,
		},
		{
			name: "astral plane uses a surrogate pair",
			row:  map[string]any{"src": "clip-\U0001F3AC.yuv"},
			want: `{"src": "clip-\ud83c\udfac.yuv"}`,
		},
		{
			name: "control characters use the short escapes",
			row:  map[string]any{"stderr": "a\nb\tc\"d\\e"},
			want: `{"stderr": "a\nb\tc\"d\\e"}`,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := MarshalSorted(tc.row)
			if err != nil {
				t.Fatalf("MarshalSorted: %v", err)
			}
			if got != tc.want {
				t.Errorf("MarshalSorted() =\n  %s\nwant\n  %s", got, tc.want)
			}
		})
	}
}
