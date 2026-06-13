// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/gpu/detect_test.go — unit tests for GPU vendor detection.
//
// The detection functions rely on subprocess probes (nvidia-smi, rocm-smi,
// clinfo, system_profiler).  We exercise the pure-Go parsing helpers
// directly rather than mocking the subprocesses, and verify that Detect()
// always returns a valid Capability even when all probes fail.
//
// ADR-0713: vmafx-node Go worker binary.

package gpu

import (
	"testing"
)

// TestNonEmptyLines checks the helper that splits probe output.
func TestNonEmptyLines(t *testing.T) {
	t.Parallel()
	got := nonEmptyLines("  a\n\n  b  \n\nc  \n")
	want := []string{"a", "b", "c"}
	if len(got) != len(want) {
		t.Fatalf("len = %d, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("got[%d] = %q, want %q", i, got[i], want[i])
		}
	}
}

// TestExtractAfterColon checks colon-split helper.
func TestExtractAfterColon(t *testing.T) {
	t.Parallel()
	cases := []struct {
		in   string
		want string
	}{
		{"GPU 0: NVIDIA GeForce RTX 4090 (UUID: ...)", "NVIDIA GeForce RTX 4090 (UUID: ...)"},
		{"no colon here", "no colon here"},
		{"key:  value  ", "value"},
		{"", ""},
	}
	for _, tc := range cases {
		got := extractAfterColon(tc.in)
		if got != tc.want {
			t.Errorf("extractAfterColon(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

// TestCPUFallback verifies that cpuFallback always yields valid output.
func TestCPUFallback(t *testing.T) {
	t.Parallel()
	cap := cpuFallback()
	if cap.Vendor != VendorCPU {
		t.Errorf("vendor = %q, want %q", cap.Vendor, VendorCPU)
	}
	if cap.DeviceCount != 0 {
		t.Errorf("DeviceCount = %d, want 0", cap.DeviceCount)
	}
	if cap.PrimaryBackend() != "cpu" {
		t.Errorf("PrimaryBackend = %q, want cpu", cap.PrimaryBackend())
	}
}

// TestCapability_PrimaryBackend checks backend priority.
func TestCapability_PrimaryBackend(t *testing.T) {
	t.Parallel()
	cases := []struct {
		backends []string
		want     string
	}{
		{[]string{"cuda", "cpu"}, "cuda"},
		{[]string{"hip", "cpu"}, "hip"},
		{[]string{"cpu"}, "cpu"},
		{nil, "cpu"},
	}
	for _, tc := range cases {
		cap := Capability{Backends: tc.backends}
		if got := cap.PrimaryBackend(); got != tc.want {
			t.Errorf("PrimaryBackend(%v) = %q, want %q", tc.backends, got, tc.want)
		}
	}
}

// TestCapability_DeviceCountStr checks string conversion.
func TestCapability_DeviceCountStr(t *testing.T) {
	t.Parallel()
	cap := Capability{DeviceCount: 4}
	if got := cap.DeviceCountStr(); got != "4" {
		t.Errorf("DeviceCountStr() = %q, want %q", got, "4")
	}
}

// TestDetect_NeverPanics verifies Detect returns a usable Capability even
// when none of the probe binaries are present on the test host.
func TestDetect_NeverPanics(t *testing.T) {
	t.Parallel()
	cap := Detect()
	// Must have a non-empty vendor string.
	if cap.Vendor == "" {
		t.Error("Detect() returned empty Vendor")
	}
	// Must include "cpu" in Backends.
	hasCPU := false
	for _, b := range cap.Backends {
		if b == "cpu" {
			hasCPU = true
			break
		}
	}
	if !hasCPU {
		t.Errorf("Detect() returned Backends %v — must include 'cpu'", cap.Backends)
	}
	// PrimaryBackend must be non-empty.
	if cap.PrimaryBackend() == "" {
		t.Error("PrimaryBackend() returned empty string")
	}
}

// TestVendorConstants confirms the constant values match what the node
// sends in RegisterNodeRequest.gpu_vendor.
func TestVendorConstants(t *testing.T) {
	t.Parallel()
	want := map[Vendor]string{
		VendorNVIDIA: "nvidia",
		VendorAMD:    "amd",
		VendorIntel:  "intel",
		VendorApple:  "apple",
		VendorCPU:    "cpu",
	}
	for v, s := range want {
		if string(v) != s {
			t.Errorf("Vendor(%q) constant = %q, want %q", v, string(v), s)
		}
	}
}
