// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package pyjson_test

import (
	"math"
	"testing"

	"github.com/VMAFx/vmafx/internal/pyjson"
)

// TestMarshal_matchesPythonJSONDumps pins the encoder against real
// json.dumps output. Every "want" below is a verbatim dump from CPython for
// the same value, so a divergence in separators, indentation, key order,
// float rendering or string escaping fails here.
func TestMarshal_matchesPythonJSONDumps(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name     string
		value    any
		sortKeys bool
		indent   string
		want     string
	}{
		{
			name: "nested value with indent=2 and sort_keys",
			value: map[string]any{
				"a": 1, "b": []any{1, 2}, "c": 93.0,
				"d": map[string]any{"e": nil},
			},
			sortKeys: true, indent: "  ",
			want: "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"c\": 93.0," +
				"\n  \"d\": {\n    \"e\": null\n  }\n}",
		},
		{
			name: "non-finite floats use Python's tokens",
			value: map[string]any{
				"x": math.NaN(), "y": math.Inf(1), "z": -0.5,
			},
			sortKeys: true, indent: "  ",
			want: "{\n  \"x\": NaN,\n  \"y\": Infinity,\n  \"z\": -0.5\n}",
		},
		{
			name: "empty containers stay on one line",
			value: map[string]any{
				"empty_obj": map[string]any{}, "empty_arr": []any{},
			},
			sortKeys: true, indent: "  ",
			want: "{\n  \"empty_arr\": [],\n  \"empty_obj\": {}\n}",
		},
		{
			name:     "strings escape like ensure_ascii=True",
			value:    map[string]any{"s": `a"b`, "u": "é"},
			sortKeys: true, indent: "  ",
			want: "{\n  \"s\": \"a\\\"b\",\n  \"u\": \"\\u00e9\"\n}",
		},
		{
			name:     "compact mode uses the default separators",
			value:    map[string]any{"b": 1, "a": 2},
			sortKeys: true,
			want:     `{"a": 2, "b": 1}`,
		},
		{
			name: "arrays of objects nest correctly",
			value: map[string]any{
				"nested": []any{
					map[string]any{"k": 1.0},
					map[string]any{"k": 2.5},
				},
			},
			sortKeys: true, indent: "  ",
			want: "{\n  \"nested\": [\n    {\n      \"k\": 1.0\n    },\n" +
				"    {\n      \"k\": 2.5\n    }\n  ]\n}",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := pyjson.Marshal(tc.value, pyjson.Options{
				SortKeys: tc.sortKeys, Indent: tc.indent,
			})
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			if string(got) != tc.want {
				t.Errorf("Marshal =\n%s\nwant\n%s", got, tc.want)
			}
		})
	}
}

// TestEncodeString_matchesPythonEscaping covers the two places encoding/json
// diverges from Python: HTML escaping and non-ASCII.
func TestEncodeString_matchesPythonEscaping(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   string
		want string
	}{
		{"plain ascii", "hello", `"hello"`},
		{"quote", `a"b`, `"a\"b"`},
		{"backslash", `a\b`, `"a\\b"`},
		{"newline", "a\nb", `"a\nb"`},
		{"tab", "a\tb", `"a\tb"`},
		{"carriage return", "a\rb", `"a\rb"`},
		{"backspace", "a\bb", `"a\bb"`},
		{"form feed", "a\fb", `"a\fb"`},
		{"other control char", "a\x01b", `"a\u0001b"`},
		{"delete is escaped", "a\x7fb", `"a\u007fb"`},
		// Python does NOT HTML-escape these; encoding/json does.
		{"angle brackets stay literal", "<a>", `"<a>"`},
		{"ampersand stays literal", "a&b", `"a&b"`},
		{"non-ascii is escaped", "é", `"\u00e9"`},
		{"astral plane uses a surrogate pair", "\U0001F600", `"\ud83d\ude00"`},
		{"ffmpeg filter fragment", "scale=w=320:h=240", `"scale=w=320:h=240"`},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := pyjson.EncodeString(tc.in); got != tc.want {
				t.Errorf("EncodeString(%q) = %s, want %s", tc.in, got, tc.want)
			}
		})
	}
}

// TestFormatFloat pins the numeric rendering.
func TestFormatFloat(t *testing.T) {
	t.Parallel()

	tests := []struct {
		in   float64
		want string
	}{
		{93.0, "93.0"},
		{93.25, "93.25"},
		{0.0, "0.0"},
		{-0.5, "-0.5"},
		{800.0, "800.0"},
		{1000000.0, "1000000.0"},
		// The exponential band edges, pinned against Python's repr.
		{1e15, "1000000000000000.0"},
		{1e16, "1e+16"},
		{1e17, "1e+17"},
		{-1e16, "-1e+16"},
		{0.0001, "0.0001"},
		{0.00001, "1e-05"},
		{9.9e-5, "9.9e-05"},
		{1.5e-7, "1.5e-07"},
		{123456789012345.0, "123456789012345.0"},
		{1234567890123456.0, "1234567890123456.0"},
		{12345678901234567.0, "1.2345678901234568e+16"},
		{math.NaN(), "NaN"},
		{math.Inf(1), "Infinity"},
		{math.Inf(-1), "-Infinity"},
	}
	for _, tc := range tests {
		t.Run(tc.want, func(t *testing.T) {
			t.Parallel()

			if got := pyjson.FormatFloat(tc.in); got != tc.want {
				t.Errorf("FormatFloat(%v) = %q, want %q", tc.in, got, tc.want)
			}
		})
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

// TestMarshal_structs covers json-tag handling on the payload shapes the
// subcommands actually emit: renamed fields, "-", omitempty, nil pointers and
// declaration-order versus sorted output.
func TestMarshal_structs(t *testing.T) {
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

		got, err := pyjson.MarshalIndent(payload, false)
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

		got, err := pyjson.Marshal(payload, pyjson.Options{SortKeys: true})
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

		got, err := pyjson.Marshal(withOptional, pyjson.Options{SortKeys: true})
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

// TestMarshal_nilContainers covers the nil slice / nil map cases.
func TestMarshal_nilContainers(t *testing.T) {
	t.Parallel()

	var nilSlice []int
	var nilMap map[string]int
	got, err := pyjson.Marshal(map[string]any{"s": nilSlice, "m": nilMap},
		pyjson.Options{SortKeys: true})
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if string(got) != `{"m": null, "s": null}` {
		t.Errorf("Marshal = %s, want nil containers as null", got)
	}
}

// TestMarshal_unsupportedMapKey rejects a non-string key rather than emitting
// something a JSON parser would refuse.
func TestMarshal_unsupportedMapKey(t *testing.T) {
	t.Parallel()

	if _, err := pyjson.Marshal(map[int]string{1: "a"}, pyjson.Options{}); err == nil {
		t.Error("expected an error for a non-string map key")
	}
}
