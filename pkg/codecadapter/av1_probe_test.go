// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package codecadapter

import (
	"errors"
	"testing"
)

// TestAv1VideoToolbox_sentinelAndProbe pins two behaviours the port lost.
//
// Python's placeholder raises a dedicated Av1VideoToolboxUnavailableError and
// lazily re-probes the host FFmpeg, so the adapter activates itself once
// upstream ships the encoder. The merged registry hard-coded an "unavailable"
// string that could never clear and returned an unmatchable fmt.Errorf.
func TestAv1VideoToolbox_sentinelAndProbe(t *testing.T) {
	t.Parallel()

	a, err := Get("av1_videotoolbox")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if a.availableFn == nil {
		t.Error("av1_videotoolbox has no runtime availability probe; it can never activate")
	}

	// On a host without the encoder the argv call must fail with the sentinel.
	if !av1VideoToolboxAvailable() {
		_, argErr := a.FFmpegCodecArgs("medium", 50)
		if !errors.Is(argErr, ErrAv1VideoToolboxUnavailable) {
			t.Errorf("error should match ErrAv1VideoToolboxUnavailable, got %v", argErr)
		}
	}
}

// TestProbeAv1VideoToolbox_failsClosed pins the "inactive on any uncertainty"
// rule: a missing or non-FFmpeg binary must not activate the placeholder.
func TestProbeAv1VideoToolbox_failsClosed(t *testing.T) {
	t.Parallel()

	for _, bin := range []string{
		"/nonexistent/ffmpeg-does-not-exist",
		"/bin/true", // exits 0, prints nothing — matches neither needle
	} {
		if probeAv1VideoToolboxAvailable(bin) {
			t.Errorf("probe(%q) = true; must fail closed", bin)
		}
	}
}
