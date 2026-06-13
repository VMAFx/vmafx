// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encoder/timeout_test.go — regression coverage for the ffmpeg
// encode-subprocess and ffprobe bitrate-probe timeouts. The prior
// runEncode and probeBitrateKbps called exec.Command (not
// exec.CommandContext) with no upper bound, so a hung child (stuck GPU
// driver, broken codec init, blocked I/O) would pin the compare /
// ladder sweep forever with no operator recovery path short of SIGKILL
// to the parent.

package encoder

import (
	"testing"
	"time"
)

func TestEncodeTimeout_DefaultWhenUnset(t *testing.T) {
	t.Setenv("VMAFX_TUNE_ENCODE_TIMEOUT", "")
	got := encodeTimeout()
	if got != defaultEncodeTimeout {
		t.Errorf("encodeTimeout() = %v, want %v (the default)", got, defaultEncodeTimeout)
	}
}

func TestEncodeTimeout_HonoursEnvOverride(t *testing.T) {
	t.Setenv("VMAFX_TUNE_ENCODE_TIMEOUT", "7s")
	got := encodeTimeout()
	if got != 7*time.Second {
		t.Errorf("encodeTimeout() = %v, want 7s", got)
	}
}

func TestEncodeTimeout_InvalidEnvFallsBackToDefault(t *testing.T) {
	t.Setenv("VMAFX_TUNE_ENCODE_TIMEOUT", "not-a-duration")
	got := encodeTimeout()
	if got != defaultEncodeTimeout {
		t.Errorf("encodeTimeout() with invalid env = %v, want default %v", got, defaultEncodeTimeout)
	}
}

func TestProbeTimeout_DefaultWhenUnset(t *testing.T) {
	t.Setenv("VMAFX_TUNE_PROBE_TIMEOUT", "")
	got := probeTimeout()
	if got != defaultProbeTimeout {
		t.Errorf("probeTimeout() = %v, want %v", got, defaultProbeTimeout)
	}
}

func TestProbeTimeout_HonoursEnvOverride(t *testing.T) {
	t.Setenv("VMAFX_TUNE_PROBE_TIMEOUT", "500ms")
	got := probeTimeout()
	if got != 500*time.Millisecond {
		t.Errorf("probeTimeout() = %v, want 500ms", got)
	}
}

// TestProbeBitrateKbps_TimeoutDoesNotHang exercises the ffprobe code path
// with an aggressive 100ms cap. The probe attempts to run a non-existent
// "ffprobe" against a non-existent path; the test only asserts that the
// call returns (does not hang) within a generous 5-second test budget.
// The return value is 0.0 (per the existing "probe failed → 0" contract).
func TestProbeBitrateKbps_TimeoutDoesNotHang(t *testing.T) {
	t.Setenv("VMAFX_TUNE_PROBE_TIMEOUT", "100ms")

	done := make(chan float64, 1)
	go func() {
		// Use a definitely-absent ffmpeg binary path; the derived ffprobe
		// path will also be absent, so exec.CommandContext returns
		// immediately with an exec error. The test guarantees the call
		// does not block indefinitely.
		done <- probeBitrateKbps("/no/such/file.mkv", "/no/such/ffmpeg-bin")
	}()
	select {
	case <-done:
		// Returned within budget; good.
	case <-time.After(5 * time.Second):
		t.Fatal("probeBitrateKbps did not return within 5s — timeout wiring broken")
	}
}
