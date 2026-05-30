// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/gpu/detect.go — GPU vendor detection for vmafx-node.
//
// Detection probes subprocess tools already present in the runtime image
// (nvidia-smi, rocm-smi, clinfo, system_profiler).  Each probe is
// independent; results are combined into a Capability struct.
//
// Probe order (fastest first):
//  1. nvidia-smi -L     → NVIDIA GPU lines
//  2. rocm-smi --showid → AMD GPU lines
//  3. clinfo            → Intel GPU via platform substring match
//  4. system_profiler   → Apple Metal (macOS only)
//  5. CPU fallback      → always succeeds
//
// ADR-0713: vmafx-node Go worker binary.

package gpu

import (
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"time"
)

// Vendor identifies the GPU vendor.
type Vendor string

const (
	VendorNVIDIA Vendor = "nvidia"
	VendorAMD    Vendor = "amd"
	VendorIntel  Vendor = "intel"
	VendorApple  Vendor = "apple"
	VendorCPU    Vendor = "cpu"
)

// Capability describes the GPU hardware available on the current host.
type Capability struct {
	// Vendor is the primary GPU vendor detected.
	Vendor Vendor

	// DeviceCount is the number of GPUs of that vendor.
	DeviceCount int

	// DeviceName is a human-readable name for the primary device.
	DeviceName string

	// ComputeCapability is the CUDA SM version string for NVIDIA GPUs
	// (e.g. "8.9" for Ada Lovelace).  Empty for non-NVIDIA vendors.
	ComputeCapability string

	// Backends lists the VMAFX backend names this node can support, ordered by
	// preference.  Always includes "cpu" as the last entry.
	Backends []string
}

// probeTimeout is the maximum wall time allowed per subprocess probe.
const probeTimeout = 5 * time.Second

// Detect probes the host for available GPU hardware and returns a Capability
// struct.  It never returns an error; on all-probe-failure it falls back to
// CPU.
func Detect() Capability {
	if cap, ok := probeNVIDIA(); ok {
		return cap
	}
	if cap, ok := probeAMD(); ok {
		return cap
	}
	if cap, ok := probeIntel(); ok {
		return cap
	}
	if runtime.GOOS == "darwin" {
		if cap, ok := probeAppleMetal(); ok {
			return cap
		}
	}
	return cpuFallback()
}

// runProbe runs cmd with args, respects probeTimeout, and returns stdout.
// Returns ("", false) on any failure.
func runProbe(cmd string, args ...string) (string, bool) {
	// #nosec G204 -- runProbe is only invoked with hard-coded vendor probe
	// commands (nvidia-smi, rocm-smi, etc.) from this package; never with
	// caller-supplied input.
	c := exec.Command(cmd, args...)
	c.WaitDelay = probeTimeout
	out, err := c.Output()
	if err != nil {
		return "", false
	}
	return string(out), true
}

// probeNVIDIA tries nvidia-smi -L to detect NVIDIA GPUs.
func probeNVIDIA() (Capability, bool) {
	out, ok := runProbe("nvidia-smi", "-L")
	if !ok {
		return Capability{}, false
	}
	lines := nonEmptyLines(out)
	if len(lines) == 0 {
		return Capability{}, false
	}
	// Extract device name from the first line, e.g.:
	//   GPU 0: NVIDIA GeForce RTX 4090 (UUID: ...)
	name := extractAfterColon(lines[0])

	// Attempt to read compute capability via nvidia-smi --query-gpu.
	computeCap := probeNVIDIAComputeCapability()

	return Capability{
		Vendor:            VendorNVIDIA,
		DeviceCount:       len(lines),
		DeviceName:        name,
		ComputeCapability: computeCap,
		Backends:          []string{"cuda", "cpu"},
	}, true
}

// probeNVIDIAComputeCapability returns the SM version for GPU 0, or "".
func probeNVIDIAComputeCapability() string {
	out, ok := runProbe("nvidia-smi",
		"--query-gpu=compute_cap",
		"--format=csv,noheader",
		"--id=0",
	)
	if !ok {
		return ""
	}
	return strings.TrimSpace(out)
}

// probeAMD tries rocm-smi --showid to detect AMD GPUs.
func probeAMD() (Capability, bool) {
	out, ok := runProbe("rocm-smi", "--showid")
	if !ok {
		return Capability{}, false
	}
	lines := nonEmptyLines(out)
	// rocm-smi output includes header lines; count GPU-prefixed lines.
	var gpuLines []string
	for _, l := range lines {
		if strings.HasPrefix(strings.TrimSpace(l), "GPU[") {
			gpuLines = append(gpuLines, l)
		}
	}
	if len(gpuLines) == 0 {
		// Try counting "GPU " lines without brackets.
		for _, l := range lines {
			if strings.Contains(strings.ToUpper(l), "GPU ") {
				gpuLines = append(gpuLines, l)
			}
		}
	}
	count := len(gpuLines)
	if count == 0 {
		count = 1 // rocm-smi returned output but we couldn't parse — assume 1
	}

	name := extractAMDDeviceName()
	return Capability{
		Vendor:      VendorAMD,
		DeviceCount: count,
		DeviceName:  name,
		Backends:    []string{"hip", "cpu"},
	}, true
}

// extractAMDDeviceName queries rocm-smi for the device name of GPU 0.
func extractAMDDeviceName() string {
	out, ok := runProbe("rocm-smi", "--showproductname")
	if !ok {
		return "AMD GPU"
	}
	for _, l := range nonEmptyLines(out) {
		if strings.Contains(strings.ToLower(l), "card series") ||
			strings.Contains(strings.ToLower(l), "card model") {
			return strings.TrimSpace(extractAfterColon(l))
		}
	}
	return "AMD GPU"
}

// probeIntel tries clinfo to detect Intel GPUs via the Intel OpenCL platform.
func probeIntel() (Capability, bool) {
	out, ok := runProbe("clinfo")
	if !ok {
		return Capability{}, false
	}
	// Look for an Intel platform entry.
	if !strings.Contains(strings.ToLower(out), "intel") {
		return Capability{}, false
	}

	// Count "Device Name" lines under an Intel platform.
	count := 0
	name := "Intel GPU"
	inIntelPlatform := false
	for _, l := range strings.Split(out, "\n") {
		lower := strings.ToLower(l)
		if strings.Contains(lower, "platform name") && strings.Contains(lower, "intel") {
			inIntelPlatform = true
		}
		if inIntelPlatform && strings.Contains(lower, "platform name") && !strings.Contains(lower, "intel") {
			inIntelPlatform = false
		}
		if inIntelPlatform && strings.Contains(lower, "device name") {
			count++
			if count == 1 {
				name = strings.TrimSpace(extractAfterColon(l))
			}
		}
	}
	if count == 0 {
		// clinfo mentions Intel but we could not isolate a device count.
		count = 1
	}
	return Capability{
		Vendor:      VendorIntel,
		DeviceCount: count,
		DeviceName:  name,
		Backends:    []string{"sycl", "cpu"},
	}, true
}

// probeAppleMetal queries system_profiler on macOS for Metal-capable GPUs.
func probeAppleMetal() (Capability, bool) {
	out, ok := runProbe("system_profiler", "SPDisplaysDataType")
	if !ok {
		return Capability{}, false
	}
	// Metal support lines look like:
	//   Metal:  Supported, feature set macOS GPUFamily2 v1
	if !strings.Contains(strings.ToLower(out), "metal") {
		return Capability{}, false
	}

	count := 0
	name := "Apple GPU"
	for _, l := range strings.Split(out, "\n") {
		lower := strings.ToLower(l)
		if strings.Contains(lower, "chipset model:") {
			count++
			if count == 1 {
				name = strings.TrimSpace(extractAfterColon(l))
			}
		}
	}
	if count == 0 {
		count = 1
	}
	return Capability{
		Vendor:      VendorApple,
		DeviceCount: count,
		DeviceName:  name,
		Backends:    []string{"metal", "cpu"},
	}, true
}

// cpuFallback returns a CPU-only Capability.
func cpuFallback() Capability {
	return Capability{
		Vendor:      VendorCPU,
		DeviceCount: 0,
		DeviceName:  "CPU",
		Backends:    []string{"cpu"},
	}
}

// nonEmptyLines splits s on newlines and returns non-empty, trimmed lines.
func nonEmptyLines(s string) []string {
	parts := strings.Split(s, "\n")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		if t := strings.TrimSpace(p); t != "" {
			out = append(out, t)
		}
	}
	return out
}

// extractAfterColon returns the portion of s after the first colon, trimmed.
func extractAfterColon(s string) string {
	idx := strings.Index(s, ":")
	if idx < 0 {
		return s
	}
	return strings.TrimSpace(s[idx+1:])
}

// DeviceCountStr returns the device count as a string (for logging).
func (c Capability) DeviceCountStr() string {
	return strconv.Itoa(c.DeviceCount)
}

// PrimaryBackend returns the first (preferred) backend in the Backends list.
func (c Capability) PrimaryBackend() string {
	if len(c.Backends) == 0 {
		return "cpu"
	}
	return c.Backends[0]
}
