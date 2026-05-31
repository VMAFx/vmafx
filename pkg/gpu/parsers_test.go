// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/gpu/parsers_test.go — unit tests that exercise the probe wrappers
// for AMD, Intel, and Apple Metal. The subprocess probes
// (rocm-smi / clinfo / system_profiler) are typically absent in the
// test environment; the tests verify graceful handling rather than
// requiring specific GPU hardware.

package gpu

import (
	"os/exec"
	"runtime"
	"testing"
)

// TestProbeAMD_BinaryAbsent verifies probeAMD returns ok=false when
// rocm-smi is not on PATH.
func TestProbeAMD_BinaryAbsent(t *testing.T) {
	t.Parallel()
	if _, ok := lookupBin("rocm-smi"); ok {
		t.Skip("rocm-smi present — skip absent-binary branch")
	}
	cap, ok := probeAMD()
	if ok {
		t.Errorf("probeAMD returned ok=true with no rocm-smi: %+v", cap)
	}
}

// TestProbeIntel_BinaryAbsent verifies probeIntel returns ok=false when
// clinfo is not on PATH.
func TestProbeIntel_BinaryAbsent(t *testing.T) {
	t.Parallel()
	if _, ok := lookupBin("clinfo"); ok {
		t.Skip("clinfo present — skip absent-binary branch")
	}
	cap, ok := probeIntel()
	if ok {
		t.Errorf("probeIntel returned ok=true with no clinfo: %+v", cap)
	}
}

// TestProbeAppleMetal_LinuxAbsent verifies the Metal probe never returns
// true on non-darwin (system_profiler is not present).
func TestProbeAppleMetal_LinuxAbsent(t *testing.T) {
	t.Parallel()
	if runtime.GOOS == "darwin" {
		t.Skip("darwin host — system_profiler probe is environment-dependent")
	}
	cap, ok := probeAppleMetal()
	if ok {
		t.Errorf("probeAppleMetal returned ok=true on non-darwin: %+v", cap)
	}
}

// TestExtractAMDDeviceName_DefaultsToGenericName verifies the fallback
// when rocm-smi is unavailable.
func TestExtractAMDDeviceName_DefaultsToGenericName(t *testing.T) {
	t.Parallel()
	if _, ok := lookupBin("rocm-smi"); ok {
		t.Skip("rocm-smi present — fallback path not exercised")
	}
	got := extractAMDDeviceName()
	if got != "AMD GPU" {
		t.Errorf("extractAMDDeviceName() = %q, want %q (fallback)", got, "AMD GPU")
	}
}

// TestRunProbe_BinaryAbsent verifies the run-probe helper returns
// ("", false) when the binary is missing.
func TestRunProbe_BinaryAbsent(t *testing.T) {
	t.Parallel()
	out, ok := runProbe("/definitely/not/a/binary/name", "--version")
	if ok {
		t.Errorf("runProbe should fail on missing binary, got ok=true (out=%q)", out)
	}
	if out != "" {
		t.Errorf("runProbe missing-binary output = %q, want empty", out)
	}
}

// TestRunProbe_Success exercises the happy path via a tiny shell builtin
// stand-in. Uses `true` which is available on every POSIX host.
func TestRunProbe_Success(t *testing.T) {
	t.Parallel()
	if _, ok := lookupBin("true"); !ok {
		t.Skip("/bin/true not available — skipping")
	}
	out, ok := runProbe("true")
	if !ok {
		t.Errorf("runProbe(true) returned ok=false (out=%q)", out)
	}
}

// TestProbeNVIDIAComputeCapability_HandlesAbsentBinary verifies the
// helper returns the empty string when nvidia-smi is absent.
func TestProbeNVIDIAComputeCapability_HandlesAbsentBinary(t *testing.T) {
	t.Parallel()
	if _, ok := lookupBin("nvidia-smi"); ok {
		t.Skip("nvidia-smi present — only the absent-binary branch is tested here")
	}
	got := probeNVIDIAComputeCapability()
	if got != "" {
		t.Errorf("probeNVIDIAComputeCapability = %q, want empty for missing binary", got)
	}
}

// TestProbeNVIDIA_HandlesAbsentBinary verifies probeNVIDIA returns
// ok=false when nvidia-smi is absent.
func TestProbeNVIDIA_HandlesAbsentBinary(t *testing.T) {
	t.Parallel()
	if _, ok := lookupBin("nvidia-smi"); ok {
		t.Skip("nvidia-smi present — only the absent-binary branch is tested here")
	}
	_, ok := probeNVIDIA()
	if ok {
		t.Error("probeNVIDIA should return ok=false when nvidia-smi is missing")
	}
}

// lookupBin returns ("", false) if the binary is not on PATH.
// Test-local helper that wraps exec.LookPath.
func lookupBin(name string) (string, bool) {
	p, err := exec.LookPath(name)
	if err != nil {
		return "", false
	}
	return p, true
}
