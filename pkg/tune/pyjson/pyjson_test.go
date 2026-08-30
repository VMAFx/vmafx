// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package pyjson

import (
	"math"
	"strings"
	"testing"
)

// TestFormatFloatMatchesCPythonRepr pins the four ways CPython's repr()
// differs from Go's %g: the mandatory ".0" on integral values, the
// fixed/exponential switch at decpt <= -4 || decpt > 16, the two-digit
// exponent, and the bare non-finite tokens.
func TestFormatFloatMatchesCPythonRepr(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   float64
		want string
	}{
		{"zero gains a .0", 0, "0.0"},
		{"negative zero keeps its sign", math.Copysign(0, -1), "-0.0"},
		{"integral value gains a .0", 1, "1.0"},
		{"integral negative", -42, "-42.0"},
		{"typical VMAF score", 93.5, "93.5"},
		{"shortest round-trip digits", 0.1, "0.1"},
		{"long mantissa", 1828.686011867366, "1828.686011867366"},
		{"seventeen-digit mantissa", 1828.6860118673662, "1828.6860118673662"},
		// The fixed/exponential switch. Go's %g flips at 1e15; CPython at 1e16.
		{"1e15 stays fixed", 1e15, "1000000000000000.0"},
		{"1e16 goes exponential", 1e16, "1e+16"},
		{"1.5e20", 1.5e20, "1.5e+20"},
		// The small end: exponential at decpt <= -4, i.e. from 1e-5 down.
		{"1e-4 stays fixed", 1e-4, "0.0001"},
		{"1e-5 goes exponential with two exponent digits", 1e-5, "1e-05"},
		{"1.25e-7", 1.25e-7, "1.25e-07"},
		{"three-digit exponent", 1e-300, "1e-300"},
		// Non-finite tokens: CPython's allow_nan=True default.
		{"NaN", math.NaN(), "NaN"},
		{"positive infinity", math.Inf(1), "Infinity"},
		{"negative infinity", math.Inf(-1), "-Infinity"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := FormatFloat(tc.in); got != tc.want {
				t.Errorf("FormatFloat(%v) = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

// TestEncodeString pins the ensure_ascii=True escaping CPython applies.
func TestEncodeString(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   string
		want string
	}{
		{"plain", "libx264", `"libx264"`},
		{"quote and backslash", `a"b\c`, `"a\"b\\c"`},
		{"short escapes", "a\nb\tc\rd\be\ff", `"a\nb\tc\rd\be\ff"`},
		{"other control chars", "\x00\x1f", `"\u0000\u001f"`},
		{"tilde is the last literal byte", "~", `"~"`},
		{"del is escaped", "\x7f", `"\u007f"`},
		{"non-ascii is escaped", "caf\u00e9", `"caf\u00e9"`},
		{"em dash", "a\u2014b", `"a\u2014b"`},
		{"astral plane uses a surrogate pair", "\U0001f3ac", `"\ud83c\udfac"`},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := EncodeString(tc.in); got != tc.want {
				t.Errorf("EncodeString(%q) = %s, want %s", tc.in, got, tc.want)
			}
		})
	}
}

// TestMarshalLayout pins the container layout: sorted keys, ": " after a key,
// "," (no trailing space) between indented items, and bare {} / [] for empties.
func TestMarshalLayout(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		in     any
		indent int
		want   string
	}{
		{"empty object", map[string]any{}, 2, "{}"},
		{"empty array", []any{}, 2, "[]"},
		{"null", nil, 2, "null"},
		{"bool", true, 2, "true"},
		{"int", 7, 2, "7"},
		{
			name:   "keys are sorted",
			in:     map[string]any{"b": 1, "a": 2, "C": 3},
			indent: 2,
			want:   "{\n  \"C\": 3,\n  \"a\": 2,\n  \"b\": 1\n}",
		},
		{
			name:   "nested indentation",
			in:     map[string]any{"outer": map[string]any{"inner": []any{1, 2}}},
			indent: 2,
			want:   "{\n  \"outer\": {\n    \"inner\": [\n      1,\n      2\n    ]\n  }\n}",
		},
		{
			name:   "compact form uses a comma-space separator",
			in:     map[string]any{"a": 1, "b": []any{2, 3}},
			indent: 0,
			want:   `{"a": 1, "b": [2, 3]}`,
		},
		{
			name:   "float64 slice fields keep repr spelling",
			in:     map[string]any{"xs": []any{1.0, 0.5, math.NaN()}},
			indent: 0,
			want:   `{"xs": [1.0, 0.5, NaN]}`,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := Marshal(tc.in, tc.indent)
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			if got != tc.want {
				t.Errorf("Marshal =\n%s\nwant\n%s", got, tc.want)
			}
		})
	}
}

// TestMarshalRejectsUnsupportedTypes checks the deliberate strictness: an
// unknown type is an error, not a silent guess, so schema drift fails in a
// test rather than emitting wrong bytes.
func TestMarshalRejectsUnsupportedTypes(t *testing.T) {
	t.Parallel()

	type custom struct{ A int }
	if _, err := Marshal(map[string]any{"x": custom{1}}, 2); err == nil {
		t.Fatal("expected an error for an unsupported type, got nil")
	}
	if _, err := Marshal(map[string]any{"x": []int{1, 2}}, 2); err == nil {
		t.Fatal("expected an error for []int (only []any and []string are handled)")
	}
}

// TestNaNToNullAndMarshalStrict pin the portable RFC-8259 path the sidecar
// state file and the executor's JSONL rows use.
func TestNaNToNullAndMarshalStrict(t *testing.T) {
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
}

// TestMustMarshalPanicsOnBadInput documents the MustMarshal contract.
func TestMustMarshalPanicsOnBadInput(t *testing.T) {
	t.Parallel()

	defer func() {
		if recover() == nil {
			t.Error("MustMarshal should panic on an unsupported type")
		}
	}()
	_ = MustMarshal(map[string]any{"x": struct{}{}}, 2)
}
