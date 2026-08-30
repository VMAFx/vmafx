// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/resolution_test.go — model-selection and CRF-offset tests.
//
// The expected values were produced by vmaftune.resolution's
// select_vmaf_model_version / neg_model_for / crf_offset_for_resolution.

package corpus

import "testing"

func TestSelectVMAFModelVersion(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		width, height int
		want          string
		wantErr       bool
	}{
		{name: "SD picks the 1080p model", width: 640, height: 480, want: Model1080P},
		{name: "720p picks the 1080p model", width: 1280, height: 720, want: Model1080P},
		{name: "1080p picks the 1080p model", width: 1920, height: 1080, want: Model1080P},
		{name: "2159 lines is still below the 4K threshold", width: 3840, height: 2159,
			want: Model1080P},
		{name: "UHD picks the 4K model", width: 3840, height: 2160, want: Model4K},
		{name: "8K picks the 4K model", width: 7680, height: 4320, want: Model4K},
		{name: "a zero dimension is rejected", width: 0, height: 1080, wantErr: true},
		{name: "a negative dimension is rejected", width: 1920, height: -1, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := SelectVMAFModelVersion(tc.width, tc.height)
			if (err != nil) != tc.wantErr {
				t.Fatalf("SelectVMAFModelVersion error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if got != tc.want {
				t.Errorf("SelectVMAFModelVersion(%d, %d) = %q, want %q",
					tc.width, tc.height, got, tc.want)
			}
		})
	}
}

func TestNegModelFor(t *testing.T) {
	t.Parallel()

	tests := []struct{ in, want string }{
		{in: Model1080P, want: Model1080PNEG},
		{in: Model4K, want: Model4KNEG},
		// Idempotent: an already-NEG model is returned unchanged.
		{in: Model1080PNEG, want: Model1080PNEG},
		{in: Model4KNEG, want: Model4KNEG},
		// A key=value override is a model-path override, not a version.
		{in: "path=/m/hdr.json", want: "path=/m/hdr.json"},
		{in: "version=vmaf_v0.6.1", want: "version=vmaf_v0.6.1"},
		// An unknown model gets the suffix so libvmaf surfaces a clear
		// missing-model error rather than silently using the wrong one.
		{in: "weird_model", want: "weird_modelneg"},
	}
	for _, tc := range tests {
		if got := NegModelFor(tc.in); got != tc.want {
			t.Errorf("NegModelFor(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestCRFOffsetForResolution(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		width, height int
		want          int
		wantErr       bool
	}{
		{name: "4K under-shoots at parity CRF", width: 3840, height: 2160, want: -2},
		{name: "1080p is the baseline", width: 1920, height: 1080, want: 0},
		{name: "720p over-shoots", width: 1280, height: 720, want: 2},
		{name: "SD over-shoots more", width: 640, height: 480, want: 4},
		{name: "a zero dimension is rejected", width: 1920, height: 0, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := CRFOffsetForResolution(tc.width, tc.height)
			if (err != nil) != tc.wantErr {
				t.Fatalf("CRFOffsetForResolution error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if got != tc.want {
				t.Errorf("CRFOffsetForResolution(%d, %d) = %d, want %d",
					tc.width, tc.height, got, tc.want)
			}
		})
	}
}
