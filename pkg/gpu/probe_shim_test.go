// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/gpu/probe_shim_test.go — exercises the AMD/Intel/Apple probe
// parsers by prepending a temp directory of fake binaries to PATH.
//
// Each shim is a tiny POSIX shell script that emits canned output that
// matches the real rocm-smi/clinfo/system_profiler textual schema, so
// the parser branches we cannot otherwise reach get covered.

package gpu

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

// writeShim writes a shell script at dir/name that echoes payload to
// stdout and exits 0. dir is prepended to PATH by the caller.
func writeShim(t *testing.T, dir, name, payload string) {
	t.Helper()
	script := "#!/bin/sh\ncat <<'__VMAFX_EOF__'\n" + payload + "\n__VMAFX_EOF__\n"
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(script), 0o700); err != nil {
		t.Fatalf("writeShim(%s): %v", name, err)
	}
}

// withPATH replaces $PATH with the supplied prefix + the original value
// and registers a cleanup that restores it.
func withPATH(t *testing.T, prefix string) {
	t.Helper()
	old := os.Getenv("PATH")
	t.Cleanup(func() {
		_ = os.Setenv("PATH", old)
	})
	if err := os.Setenv("PATH", prefix+string(os.PathListSeparator)+old); err != nil {
		t.Fatalf("Setenv PATH: %v", err)
	}
}

// TestProbeAMD_WithShim runs the probe against canned rocm-smi output.
func TestProbeAMD_WithShim(t *testing.T) {
	// Mutates PATH — cannot run in parallel with other PATH-mutating tests.
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	// Realistic rocm-smi --showid output: header + per-GPU lines.
	writeShim(t, dir, "rocm-smi", `========================ROCm System Management Interface========================
================================================================================
GPU[0]		: GPU ID: 0xabcd
GPU[1]		: GPU ID: 0xef01
================================================================================
====================================End of ROCm SMI Log========================
`)
	// rocm-smi --showproductname response (the second invocation from
	// extractAMDDeviceName) — same shim handles both probe calls.
	// Note: the shim ignores its arguments, so the same payload appears
	// for --showproductname too. extractAMDDeviceName looks for
	// "card series" or "card model" lines; payload above lacks those
	// keys so it exercises the fallback "AMD GPU" branch.
	withPATH(t, dir)

	cap, ok := probeAMD()
	if !ok {
		t.Fatal("probeAMD ok=false with shim — expected true")
	}
	if cap.Vendor != VendorAMD {
		t.Errorf("vendor = %q, want %q", cap.Vendor, VendorAMD)
	}
	if cap.DeviceCount == 0 {
		t.Error("DeviceCount = 0 — expected at least 1")
	}
	if cap.PrimaryBackend() != "hip" {
		t.Errorf("PrimaryBackend = %q, want hip", cap.PrimaryBackend())
	}
}

// TestProbeAMD_ShimWithProductName exercises extractAMDDeviceName's
// "card model" branch.
func TestProbeAMD_ShimWithProductName(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	// Combined --showid + --showproductname payload (the shim ignores
	// args so both invocations get the same blob).
	writeShim(t, dir, "rocm-smi", `========================ROCm SMI========================
GPU[0]		: GPU ID: 0xabcd
GPU[0]		: Card series: Radeon RX 7900 XTX
GPU[0]		: Card model: 0x0123
=========================================================
`)
	withPATH(t, dir)
	cap, ok := probeAMD()
	if !ok {
		t.Fatal("probeAMD ok=false with product-name shim")
	}
	if cap.DeviceName == "" {
		t.Error("DeviceName is empty — expected populated")
	}
}

// TestProbeIntel_WithShim verifies the clinfo parser handles Intel
// platform / device extraction.
func TestProbeIntel_WithShim(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	writeShim(t, dir, "clinfo", `Platform Name                                   Intel(R) OpenCL Graphics
  Device Name                                   Intel(R) Arc(TM) A770 Graphics
  Device Vendor                                 Intel(R) Corporation
Platform Name                                   AMD Accelerated Parallel Processing
  Device Name                                   gfx1100
`)
	withPATH(t, dir)

	cap, ok := probeIntel()
	if !ok {
		t.Fatal("probeIntel ok=false with shim")
	}
	if cap.Vendor != VendorIntel {
		t.Errorf("vendor = %q, want %q", cap.Vendor, VendorIntel)
	}
	if cap.DeviceCount == 0 {
		t.Error("DeviceCount = 0 — expected at least 1")
	}
}

// TestProbeIntel_NoIntelPlatform exercises the early return when clinfo
// emits output without an Intel platform.
func TestProbeIntel_NoIntelPlatform(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	writeShim(t, dir, "clinfo", `Platform Name                                   AMD Accelerated Parallel Processing
  Device Name                                   gfx1100
`)
	withPATH(t, dir)

	cap, ok := probeIntel()
	if ok {
		t.Errorf("probeIntel should return false when no Intel platform present: %+v", cap)
	}
}

// TestProbeAppleMetal_LinuxShim exercises the parser on Linux too by
// shadowing system_profiler with a shim. The Detect() wrapper guards
// this for darwin only, but the parser itself is platform-agnostic.
func TestProbeAppleMetal_LinuxShim(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	writeShim(t, dir, "system_profiler", `Graphics/Displays:
    Apple M3 Max:
      Chipset Model: Apple M3 Max
      Metal:  Supported, feature set macOS GPUFamily2 v1
`)
	withPATH(t, dir)

	cap, ok := probeAppleMetal()
	if !ok {
		t.Fatal("probeAppleMetal ok=false with shim")
	}
	if cap.Vendor != VendorApple {
		t.Errorf("vendor = %q, want %q", cap.Vendor, VendorApple)
	}
	if cap.PrimaryBackend() != "metal" {
		t.Errorf("PrimaryBackend = %q, want metal", cap.PrimaryBackend())
	}
}

// TestProbeAppleMetal_NoMetalLine exercises the early return when
// system_profiler output lacks any Metal mention.
func TestProbeAppleMetal_NoMetalLine(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	writeShim(t, dir, "system_profiler", `Graphics/Displays:
    Some unrelated hardware listing
`)
	withPATH(t, dir)

	_, ok := probeAppleMetal()
	if ok {
		t.Error("probeAppleMetal should return false when 'metal' line absent")
	}
}

// TestProbeNVIDIA_WithShim covers the happy path on a non-NVIDIA host.
func TestProbeNVIDIA_WithShim(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	writeShim(t, dir, "nvidia-smi", `GPU 0: NVIDIA GeForce RTX 4090 (UUID: GPU-abc-def)
8.9
`)
	withPATH(t, dir)

	cap, ok := probeNVIDIA()
	if !ok {
		t.Fatal("probeNVIDIA ok=false with shim")
	}
	if cap.Vendor != VendorNVIDIA {
		t.Errorf("vendor = %q, want %q", cap.Vendor, VendorNVIDIA)
	}
	if cap.DeviceName == "" {
		t.Error("DeviceName empty")
	}
}

// TestDetect_PrefersNVIDIA verifies the probe-order short-circuit by
// shadowing nvidia-smi and making sure Detect picks NVIDIA before AMD.
func TestDetect_PrefersNVIDIA(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell shims require POSIX")
	}
	dir := t.TempDir()
	writeShim(t, dir, "nvidia-smi", `GPU 0: NVIDIA Shim GPU (UUID: x)
8.9
`)
	writeShim(t, dir, "rocm-smi", `GPU[0]		: GPU ID: 0xfff
`)
	withPATH(t, dir)

	cap := Detect()
	if cap.Vendor != VendorNVIDIA {
		t.Errorf("Detect preferred %q, want NVIDIA (NVIDIA probe should win)", cap.Vendor)
	}
}
