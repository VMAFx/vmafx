// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package model_test

import (
	"testing"

	"github.com/VMAFx/vmafx/pkg/model"
)

func TestCLIArgument(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   string
		want string
	}{
		{name: "bare version", in: "vmaf_v0.6.1", want: "version=vmaf_v0.6.1"},
		{name: "path selector", in: "path=/models/hdr.json", want: "path=/models/hdr.json"},
		{name: "version selector", in: "version=vmaf_4k_v0.6.1", want: "version=vmaf_4k_v0.6.1"},
		{name: "empty without default", in: "", want: "version="},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := model.CLIArgument(tc.in); got != tc.want {
				t.Fatalf("CLIArgument(%q) = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

func TestCLIArgumentOrDefault(t *testing.T) {
	t.Parallel()

	want := "version=" + model.DefaultVersion
	if got := model.CLIArgumentOrDefault(""); got != want {
		t.Fatalf("CLIArgumentOrDefault(\"\") = %q, want %q", got, want)
	}
}
