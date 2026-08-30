// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/indent_test.go — parity tests for the indented json.dumps form.
//
// The expected strings were produced by running
// json.dumps(payload, indent=2, sort_keys=True) under CPython 3 and pasting the
// output verbatim, so the table is evidence of parity rather than a restatement
// of the Go implementation.

package pyjson

import "testing"

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
		{
			name: "empty object",
			obj:  map[string]any{},
			want: "{}",
		},
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

func TestMarshalIndentSortedRejectsUnsupportedTypes(t *testing.T) {
	t.Parallel()

	if _, err := MarshalIndentSorted(map[string]any{"x": make(chan int)}, 2); err == nil {
		t.Fatal("MarshalIndentSorted accepted an unsupported value type")
	}
}
