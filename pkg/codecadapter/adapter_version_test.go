// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package codecadapter

import "testing"

// TestAdapterVersionMatchesPython pins the ADR-0298 cache-key field against the
// Python registry. Only seven adapters carry an adapter_version there; the rest
// have no such attribute, and an empty string is the faithful representation.
//
// The port integration kept a registry that set this on libx264 alone, so a
// change to any other adapter's argv shape would not have invalidated the
// content-addressed encode cache that keys on it.
func TestAdapterVersionMatchesPython(t *testing.T) {
	t.Parallel()

	want := map[string]string{
		"libx264": "2", "libvpx-vp9": "1", "libvvenc": "2",
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
