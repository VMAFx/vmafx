// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

// Package scorebackend resolves the libvmaf scoring backend a tuning run
// should use, honouring an operator preference against what the local vmaf
// binary and host hardware can actually provide.
//
// Go port of the selection half of
// tools/vmaf-tune/src/vmaftune/score_backend.py (ADR-0299 introduced
// --score-backend; ADR-0667 set the native-first "auto" priority and the
// strict explicit-request rule). The NR-proxy half of that module
// (NRProxyBackend) drives an
// ONNX model through onnxruntime and is NOT ported — see the package
// documentation in pkg/pershot for the blocker.
//
// Two questions are answered independently, exactly as the Python does:
//
//  1. Does the local vmaf binary advertise the backend? Parsed out of the
//     "--backend $name: ... auto|cpu|cuda|sycl|hip" line in `vmaf --help`.
//  2. Is the corresponding hardware reachable? Probed with nvidia-smi /
//     sycl-ls / rocminfo + rocm-smi.
//
// Relationship to pkg/gpu: pkg/gpu.Detect answers a different question — it
// returns the *first* vendor found so a vmafx-node can advertise a primary
// backend. This package probes every vendor independently, because a host
// with both an NVIDIA and an Intel GPU must be able to honour an explicit
// "--score-backend sycl" request that pkg/gpu.Detect would never surface.
package scorebackend

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"strings"
	"time"
)

// All lists the backends the vmaf CLI accepts via "--backend NAME".
// Mirrors score_backend.ALL_BACKENDS, order included.
var All = []string{"cpu", "cuda", "sycl", "hip"}

// DefaultFallbacks is the preference order walked by "auto": native vendor
// backends first in vendor-priority order, CPU as the always-available floor.
// Mirrors score_backend.DEFAULT_FALLBACKS.
var DefaultFallbacks = []string{"cuda", "sycl", "hip", "cpu"}

// probeTimeout bounds each hardware-probe subprocess. Mirrors the Python
// timeout=5 on every probe call.
const probeTimeout = 5 * time.Second

// helpTimeout bounds the `vmaf --help` invocation. The Python has no explicit
// timeout there; a bound is added because a wedged vmaf binary would
// otherwise hang backend resolution before any work starts.
const helpTimeout = 15 * time.Second

// ErrUnavailable is returned by Select when the operator explicitly requested
// a backend the host cannot provide. Never returned for prefer == "auto",
// which falls back instead. Mirrors BackendUnavailableError.
var ErrUnavailable = errors.New("requested score backend is unavailable on this host")

// Runner runs a command and returns its combined stdout+stderr plus whether
// it exited zero. It is the subprocess seam: tests inject a stub, production
// callers leave Options.Runner nil to get the real exec.
type Runner func(ctx context.Context, name string, args ...string) (out string, ok bool)

// Options configures backend detection.
type Options struct {
	// VMAFBin is the vmaf binary probed for "--backend" support.
	// Defaults to "vmaf" (PATH lookup).
	VMAFBin string

	// Runner is the subprocess seam. Defaults to execRunner.
	Runner Runner

	// Available, when non-nil, short-circuits hardware + binary probing and
	// is used as the availability list verbatim. Mirrors the Python
	// select_backend(available=[...]) test seam.
	Available []string
}

func (o Options) vmafBin() string {
	if o.VMAFBin == "" {
		return "vmaf"
	}
	return o.VMAFBin
}

func (o Options) runner() Runner {
	if o.Runner == nil {
		return execRunner
	}
	return o.Runner
}

// execRunner is the production Runner: runs name with args under a timeout
// and returns combined output plus a zero-exit flag. A missing binary, a
// non-zero exit and a timeout are all reported as ok == false.
func execRunner(ctx context.Context, name string, args ...string) (string, bool) {
	tctx, cancel := context.WithTimeout(ctx, probeTimeout)
	defer cancel()
	// #nosec G204 -- name/args come only from this package's hard-coded probe
	// commands and the operator-configured vmaf binary path.
	cmd := exec.CommandContext(tctx, name, args...)
	cmd.WaitDelay = time.Second
	out, err := cmd.CombinedOutput()
	return string(out), err == nil
}

// ParseSupported extracts the backends a vmaf binary advertises from its
// --help output.
//
// The fork's CLI prints a line shaped like
//
//	--backend $name:  exclusive backend selector — auto|cpu|cuda|sycl|hip.
//
// so each backend token is matched only when it is bounded by a pipe on the
// left and a pipe / period / whitespace / end-of-line on the right. That
// avoids false positives on prose mentions of "CUDA". "cpu" is added
// unconditionally: every libvmaf build has a CPU path. Mirrors
// score_backend.parse_supported_backends.
func ParseSupported(helpText string) map[string]bool {
	found := map[string]bool{"cpu": true}
	for _, backend := range All {
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

// vmafHelp returns the vmaf binary's --help output (stdout and stderr
// joined). Returns "" on any failure so probing degrades to "this build has
// no GPU backends" instead of erroring out.
func vmafHelp(ctx context.Context, opts Options) string {
	bin := opts.vmafBin()
	if !strings.Contains(bin, "/") {
		if _, err := exec.LookPath(bin); err != nil {
			return ""
		}
	}
	hctx, cancel := context.WithTimeout(ctx, helpTimeout)
	defer cancel()
	out, _ := opts.runner()(hctx, bin, "--help")
	// The Python joins stdout+stderr regardless of exit status; --help exits
	// non-zero on some builds, so the ok flag is deliberately ignored here.
	return out
}

// probeCUDA reports whether an NVIDIA device is reachable via nvidia-smi -L.
func probeCUDA(ctx context.Context, run Runner) bool {
	if _, err := exec.LookPath("nvidia-smi"); err != nil {
		return false
	}
	out, ok := run(ctx, "nvidia-smi", "-L")
	return ok && strings.Contains(out, "GPU")
}

// probeSYCL reports whether a SYCL GPU device is reachable via sycl-ls.
// sycl-ls prints one bracketed line per device ("[opencl:gpu]",
// "[ext_oneapi_level_zero:gpu]", ...); a CPU-only OpenCL runtime must not
// count, hence the ":gpu" check.
func probeSYCL(ctx context.Context, run Runner) bool {
	if _, err := exec.LookPath("sycl-ls"); err != nil {
		return false
	}
	out, ok := run(ctx, "sycl-ls")
	if !ok {
		return false
	}
	return strings.Contains(out, "[") && strings.Contains(strings.ToLower(out), ":gpu")
}

// probeHIP reports whether an AMD ROCm/HIP device is reachable. rocminfo is
// tried first (its "gfx" ISA string is the strongest signal); rocm-smi's
// product-name query is the fallback.
func probeHIP(ctx context.Context, run Runner) bool {
	if _, err := exec.LookPath("rocminfo"); err == nil {
		if out, ok := run(ctx, "rocminfo"); ok &&
			strings.Contains(strings.ToLower(out), "gfx") {
			return true
		}
	}
	if _, err := exec.LookPath("rocm-smi"); err != nil {
		return false
	}
	out, ok := run(ctx, "rocm-smi", "--showproductname")
	if !ok {
		return false
	}
	lower := strings.ToLower(out)
	return strings.Contains(lower, "gpu") || strings.Contains(lower, "card series")
}

// DetectAvailable returns the backends usable on this host, in All order.
//
// "Usable" means both that the local vmaf binary advertises the backend and
// that the matching hardware probe succeeds. CPU is always present.
// Mirrors score_backend.detect_available_backends.
func DetectAvailable(ctx context.Context, opts Options) []string {
	if opts.Available != nil {
		return append([]string(nil), opts.Available...)
	}
	supported := ParseSupported(vmafHelp(ctx, opts))
	run := opts.runner()

	usable := map[string]bool{"cpu": true}
	if supported["cuda"] {
		usable["cuda"] = probeCUDA(ctx, run)
	}
	if supported["sycl"] {
		usable["sycl"] = probeSYCL(ctx, run)
	}
	if supported["hip"] {
		usable["hip"] = probeHIP(ctx, run)
	}

	out := make([]string, 0, len(All))
	for _, b := range All {
		if supported[b] && usable[b] {
			out = append(out, b)
		}
	}
	return out
}

// Select resolves the backend to use.
//
//   - prefer == "auto" walks DefaultFallbacks and returns the first entry
//     present in the availability list, falling back to "cpu".
//   - Any other value is honoured strictly: an unavailable backend returns an
//     error wrapping ErrUnavailable rather than silently downgrading, because
//     a silent downgrade masks a build/driver mismatch and lies to the
//     operator about wall-clock expectations (ADR-0667).
//
// The returned string is "" only alongside a non-nil error.
func Select(ctx context.Context, prefer string, opts Options) (string, error) {
	if prefer == "" {
		prefer = "auto"
	}
	if prefer != "auto" && !contains(All, prefer) {
		return "", fmt.Errorf("unknown backend %q; expected one of: auto, %s",
			prefer, strings.Join(All, ", "))
	}

	available := DetectAvailable(ctx, opts)

	if prefer == "auto" {
		for _, candidate := range DefaultFallbacks {
			if contains(available, candidate) {
				return candidate, nil
			}
		}
		// Last-ditch: cpu is universally available even if probes failed.
		return "cpu", nil
	}

	if contains(available, prefer) {
		return prefer, nil
	}

	listed := strings.Join(available, ", ")
	if listed == "" {
		listed = "cpu"
	}
	return "", fmt.Errorf(
		"%w: backend %q requested but not available on this host (available: %s). "+
			"Check that the local vmaf binary was built with the matching backend "+
			"support and the corresponding runtime/driver is installed",
		ErrUnavailable, prefer, listed)
}

// contains reports whether xs holds s.
func contains(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
}
