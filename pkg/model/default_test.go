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
		name  string
		model string
		want  string
	}{
		{name: "bare version", model: "vmaf_v0.6.1", want: "version=vmaf_v0.6.1"},
		{name: "path selector", model: "path=/models/custom.json", want: "path=/models/custom.json"},
		{name: "formatted version", model: "version=vmaf_4k_v0.6.1", want: "version=vmaf_4k_v0.6.1"},
		{name: "empty remains an empty version selector", model: "", want: "version="},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := model.CLIArgument(tc.model); got != tc.want {
				t.Errorf("CLIArgument(%q) = %q, want %q", tc.model, got, tc.want)
			}
		})
	}
}

func TestCLIArgumentOrDefault(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		model string
		want  string
	}{
		{name: "empty uses production default", want: "version=" + model.DefaultVersion},
		{name: "bare version", model: "vmaf_v0.6.1neg", want: "version=vmaf_v0.6.1neg"},
		{name: "path selector", model: "path=/models/custom.json", want: "path=/models/custom.json"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := model.CLIArgumentOrDefault(tc.model); got != tc.want {
				t.Errorf("CLIArgumentOrDefault(%q) = %q, want %q", tc.model, got, tc.want)
			}
		})
	}
}
