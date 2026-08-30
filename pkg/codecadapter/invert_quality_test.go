// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package codecadapter

import "testing"

// TestInvertQualityMatchesPython pins each adapter's quality direction against
// the Python registry. VideoToolbox encoders use -q:v, where a HIGHER value is
// HIGHER quality (invert_quality=False in codec_adapters/*.py); every CRF/CQ/QP
// codec is inverted. The registry kept by the port integration had all four
// VideoToolbox codecs marked inverted, which flips the direction of every
// quality search that consults the field.
func TestInvertQualityMatchesPython(t *testing.T) {
	t.Parallel()

	// Mirrors get_adapter(<name>).invert_quality in the Python originals.
	want := map[string]bool{
		"libx264": true, "libx265": true, "libaom-av1": true, "libsvtav1": true,
		"libvpx-vp9": true, "libvvenc": true,
		"h264_nvenc": true, "hevc_nvenc": true, "av1_nvenc": true,
		"h264_amf": true, "hevc_amf": true, "av1_amf": true,
		"h264_qsv": true, "hevc_qsv": true, "av1_qsv": true,
		"h264_videotoolbox": false, "hevc_videotoolbox": false,
		"av1_videotoolbox": false, "prores_videotoolbox": false,
	}
	for name, expect := range want {
		a, err := Get(name)
		if err != nil {
			t.Errorf("Get(%q): %v", name, err)
			continue
		}
		if a.InvertQuality != expect {
			t.Errorf("%s InvertQuality = %v, want %v (per the Python adapter)",
				name, a.InvertQuality, expect)
		}
	}
}
