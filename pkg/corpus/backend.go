// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/backend.go — libvmaf scoring-backend selection.
//
// Go port of the selection half of vmaftune.score_backend (ADR-0299 /
// ADR-0314; ADR-0726 dropped the Vulkan backend). "auto" walks the fallback
// chain and returns the first usable entry; any explicit backend name is
// honoured strictly and errors out when the host cannot provide it — silently
// downgrading would mask hardware / build mismatches and lie to the operator
// about wall-clock expectations.
//
// Reuse note: pkg/gpu.Detect() already probes GPU vendors for vmafx-node, but
// it answers a different question (which vendor's device is present, via
// clinfo for Intel) than this selector needs (does the local vmaf binary
// advertise --backend NAME, and does the matching runtime probe — sycl-ls for
// SYCL, rocminfo/rocm-smi for HIP — succeed). Reusing it would change which
// backend the sweep picks relative to the Python implementation, so the
// vmaf-tune probe set is ported verbatim here.

package corpus

import (
	"context"
	"fmt"
	"os/exec"
	"strings"
)

// AllBackends are the backends the vmaf CLI accepts via --backend NAME.
var AllBackends = []string{"cpu", "cuda", "sycl", "hip"}

// DefaultFallbacks is the "auto" chain. Native vendor backends are preferred in
// vendor-priority order on their respective silicon; CPU is the
// always-available floor.
var DefaultFallbacks = []string{"cuda", "sycl", "hip", "cpu"}

// BackendUnavailableError reports that the user explicitly requested a backend
// the host cannot provide. It is never returned by "auto" selection — that path
// falls back.
type BackendUnavailableError struct {
	Requested string
	Available []string
}

func (e *BackendUnavailableError) Error() string {
	avail := strings.Join(e.Available, ", ")
	if avail == "" {
		avail = "cpu"
	}
	return fmt.Sprintf(
		"backend %q requested but not available on this host (available: %s). "+
			"Check that the local vmaf binary was built with the matching backend "+
			"support and the corresponding runtime/driver is installed.",
		e.Requested, avail)
}

// vmafHelp returns the vmaf --help output (stdout and stderr joined).
// It returns "" on any error so the probe logic degrades to "binary does not
// support GPU backends" rather than failing.
func vmafHelp(ctx context.Context, vmafBin string, run Runner) string {
	if run == nil {
		if !strings.Contains(vmafBin, "/") {
			if _, err := exec.LookPath(vmafBin); err != nil {
				return ""
			}
		}
		run = ExecRunner
	}
	res := run(ctx, []string{vmafBin, "--help"})
	return res.Stdout + "\n" + res.Stderr
}

// ParseSupportedBackends extracts the backends the vmaf binary advertises from
// its --help text.
//
// The fork's CLI prints a line like:
//
//	--backend $name:  exclusive backend selector — auto|cpu|cuda|sycl|hip.
//
// The alternation is matched token-wise so "cuda" inside a prose comment does
// not produce a false positive. cpu is added unconditionally — every build has
// a CPU path even when the help line is missing.
func ParseSupportedBackends(helpText string) map[string]bool {
	found := map[string]bool{"cpu": true}
	for _, backend := range AllBackends {
		for _, needle := range []string{
			"|" + backend + "|",
			"|" + backend + ".",
			"|" + backend + "\n",
			"|" + backend + " ",
		} {
			if strings.Contains(helpText, needle) {
				found[backend] = true
				break
			}
		}
	}
	return found
}

// probeCUDA reports whether a CUDA device is reachable, via "nvidia-smi -L".
func probeCUDA(ctx context.Context, run Runner) bool {
	if run == nil {
		if _, err := exec.LookPath("nvidia-smi"); err != nil {
			return false
		}
		run = ExecRunner
	}
	res := run(ctx, []string{"nvidia-smi", "-L"})
	return res.ReturnCode == 0 && strings.Contains(res.Stdout, "GPU")
}

// probeSYCL reports whether a SYCL device is reachable, via "sycl-ls".
//
// sycl-ls prints one line per device prefixed with a bracketed backend token
// ("[opencl:gpu]", "[ext_oneapi_level_zero:gpu]", ...).
func probeSYCL(ctx context.Context, run Runner) bool {
	if run == nil {
		if _, err := exec.LookPath("sycl-ls"); err != nil {
			return false
		}
		run = ExecRunner
	}
	res := run(ctx, []string{"sycl-ls"})
	return res.ReturnCode == 0 &&
		strings.Contains(res.Stdout, "[") &&
		strings.Contains(strings.ToLower(res.Stdout), ":gpu")
}

// probeHIP reports whether an AMD ROCm / HIP GPU is reachable. It tries
// rocminfo first, then rocm-smi.
func probeHIP(ctx context.Context, run Runner) bool {
	realRun := run
	haveRocminfo := true
	haveRocmSMI := true
	if realRun == nil {
		realRun = ExecRunner
		if _, err := exec.LookPath("rocminfo"); err != nil {
			haveRocminfo = false
		}
		if _, err := exec.LookPath("rocm-smi"); err != nil {
			haveRocmSMI = false
		}
	}
	if haveRocminfo {
		res := realRun(ctx, []string{"rocminfo"})
		out := strings.ToLower(res.Stdout + "\n" + res.Stderr)
		if res.ReturnCode == 0 && strings.Contains(out, "gfx") {
			return true
		}
	}
	if !haveRocmSMI {
		return false
	}
	res := realRun(ctx, []string{"rocm-smi", "--showproductname"})
	out := strings.ToLower(res.Stdout + "\n" + res.Stderr)
	return res.ReturnCode == 0 && (strings.Contains(out, "gpu") ||
		strings.Contains(out, "card series"))
}

// DetectAvailableBackends returns the backends usable on this host, in
// AllBackends order.
//
// "Usable" means both that the local vmaf binary advertises --backend NAME and
// that the corresponding hardware / runtime probe succeeds. cpu is always
// present.
func DetectAvailableBackends(ctx context.Context, vmafBin string, run Runner) []string {
	if vmafBin == "" {
		vmafBin = "vmaf"
	}
	supported := ParseSupportedBackends(vmafHelp(ctx, vmafBin, run))
	probes := map[string]bool{"cpu": true}
	if supported["cuda"] {
		probes["cuda"] = probeCUDA(ctx, run)
	}
	if supported["sycl"] {
		probes["sycl"] = probeSYCL(ctx, run)
	}
	if supported["hip"] {
		probes["hip"] = probeHIP(ctx, run)
	}
	var out []string
	for _, b := range AllBackends {
		if supported[b] && probes[b] {
			out = append(out, b)
		}
	}
	return out
}

// SelectBackend picks a backend honouring the user preference and host
// capability.
//
//   - prefer == "auto" walks fallbacks and returns the first entry present in
//     available; cpu is the universal last-ditch answer.
//   - Any other prefer value is honoured strictly: a backend absent from
//     available yields a *BackendUnavailableError.
//
// available == nil defaults to DetectAvailableBackends; tests inject a literal
// slice to keep the unit boundary tight.
func SelectBackend(
	ctx context.Context, prefer string, fallbacks, available []string, vmafBin string, run Runner,
) (string, error) {
	valid := prefer == "auto"
	for _, b := range AllBackends {
		if prefer == b {
			valid = true
			break
		}
	}
	if !valid {
		return "", fmt.Errorf("unknown backend %q; expected one of: auto, %s",
			prefer, strings.Join(AllBackends, ", "))
	}
	if fallbacks == nil {
		fallbacks = DefaultFallbacks
	}
	if available == nil {
		available = DetectAvailableBackends(ctx, vmafBin, run)
	}
	inAvailable := func(name string) bool {
		for _, a := range available {
			if a == name {
				return true
			}
		}
		return false
	}
	if prefer == "auto" {
		for _, candidate := range fallbacks {
			if inAvailable(candidate) {
				return candidate, nil
			}
		}
		// Last-ditch: cpu is universally available even if probes failed.
		return "cpu", nil
	}
	if inAvailable(prefer) {
		return prefer, nil
	}
	return "", &BackendUnavailableError{Requested: prefer, Available: available}
}
