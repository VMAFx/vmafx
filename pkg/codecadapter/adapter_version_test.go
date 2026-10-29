// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package codecadapter

import "testing"

// TestAdapterVersionMatchesPython pins the ADR-0298 cache-key field against the
// Python registry. Every Python adapter carries an adapter_version; the Go
// port takes it from the encoder registry (ADR-3388), which the Python test
// tools/vmaf-tune/tests/test_encoder_registry.py holds equal to the adapters.
//
// The port integration kept a registry that set this on libx264 alone, and
// later on a subset; eight adapters (NVENC, AMF, libaom-av1, libsvtav1) had
// an empty Go value while Python carried "1" or "2", so the two encode caches
// keyed the same encode differently.
func TestAdapterVersionMatchesPython(t *testing.T) {
	t.Parallel()

	want := map[string]string{
		"libx264": "2", "libx265": "2", "libvpx-vp9": "1", "libvvenc": "2",
		"libaom-av1": "1", "libsvtav1": "1",
		"h264_nvenc": "1", "hevc_nvenc": "1", "av1_nvenc": "1",
		"h264_amf": "2", "hevc_amf": "2", "av1_amf": "2",
		"h264_qsv": "4", "hevc_qsv": "4", "av1_qsv": "4",
		"h264_videotoolbox": "1", "hevc_videotoolbox": "1",
		"prores_videotoolbox": "1", "av1_videotoolbox": "0-placeholder",
	}
	for _, n := range Known() {
		a, err := Get(n)
		if err != nil {
			t.Errorf("Get(%q): %v", n, err)
			continue
		}
		if got, exp := a.AdapterVersion, want[n]; got != exp {
			t.Errorf("%s AdapterVersion = %q, want %q (per the Python adapter)", n, got, exp)
		}
	}
}
