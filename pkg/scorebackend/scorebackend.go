// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package scorebackend is the Go port of the backend-selection half of
// tools/vmaf-tune/src/vmaftune/score_backend.py.
//
// It answers one question: which libvmaf scoring backend (cpu / cuda / sycl /
// hip) should this run use? "Usable" means both
//
//  1. the local vmaf binary advertises --backend NAME support in its --help
//     output, and
//  2. the corresponding hardware/runtime probe succeeds
//     (nvidia-smi / sycl-ls / rocminfo|rocm-smi).
//
// prefer="auto" walks the fallback chain and returns the first usable entry;
// any explicit preference is honoured strictly and fails with an
// *UnavailableError rather than silently downgrading, because a silent
// downgrade masks hardware/build mismatches and lies to the operator about
// wall-clock expectations.
//
// Scope note: score_backend.py also hosts NRProxyBackend (the ADR-0624 /
// ADR-0615 no-reference ONNX pre-scorer). That half is not ported here — it
// needs a multi-input ONNX Runtime session, which the Go tree does not yet
// have (see pkg/fast's proxy blocker and pkg/ai.Registry.InferDirect).
package scorebackend

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"strings"
	"time"
)

// AllBackends lists the backends the vmaf CLI accepts via --backend NAME,
// in the canonical order used for reporting.
func AllBackends() []string { return []string{"cpu", "cuda", "sycl", "hip"} }

// DefaultFallbacks is the fallback chain for prefer="auto". Native vendor
// backends are preferred in vendor-priority order on their respective
// silicon; cpu is the always-available floor.
func DefaultFallbacks() []string { return []string{"cuda", "sycl", "hip", "cpu"} }

// probeTimeout bounds each hardware-probe subprocess. Mirrors the Python
// `timeout=5` on every subprocess.run probe call.
const probeTimeout = 5 * time.Second

// UnavailableError reports that the operator explicitly requested a backend
// this host cannot provide. Selection never silently downgrades.
type UnavailableError struct {
	// Requested is the backend the operator asked for.
	Requested string
	// Available is the set of backends the host can actually run.
	Available []string
}

func (e *UnavailableError) Error() string {
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

// Runner runs a probe command and returns its combined stdout, stderr and
// exit-success flag. Tests inject a fake; production callers leave it nil and
// get execRunner.
type Runner func(ctx context.Context, name string, args ...string) (stdout, stderr string, ok bool)

// Options configures Detect / Select. The zero value is the production
// configuration.
type Options struct {
	// VMAFBin is the libvmaf CLI to probe for --backend support.
	// Empty selects "vmaf" (PATH lookup).
	VMAFBin string
	// Fallbacks overrides the prefer="auto" chain. Empty selects
	// DefaultFallbacks.
	Fallbacks []string
	// Available short-circuits host detection with a literal list. Used by
	// tests to keep the unit boundary tight; nil runs Detect.
	Available []string
	// Run overrides subprocess execution. nil selects execRunner.
	Run Runner
	// LookPath overrides binary discovery. nil selects exec.LookPath.
	LookPath func(string) (string, error)
}

func (o Options) vmafBin() string {
	if o.VMAFBin == "" {
		return "vmaf"
	}
	return o.VMAFBin
}

func (o Options) fallbacks() []string {
	if len(o.Fallbacks) == 0 {
		return DefaultFallbacks()
	}
	return o.Fallbacks
}

func (o Options) runner() Runner {
	if o.Run == nil {
		return execRunner
	}
	return o.Run
}

func (o Options) lookPath() func(string) (string, error) {
	if o.LookPath == nil {
		return exec.LookPath
	}
	return o.LookPath
}

// execRunner is the production Runner: it executes name with args under a
// probeTimeout deadline and reports (stdout, stderr, exit==0).
func execRunner(ctx context.Context, name string, args ...string) (string, string, bool) {
	ctx, cancel := context.WithTimeout(ctx, probeTimeout)
	defer cancel()
	// #nosec G204 -- name/args are fixed probe literals chosen by this
	// package (nvidia-smi, sycl-ls, rocminfo, rocm-smi) or the
	// operator-configured vmaf binary. ctx enforces probeTimeout.
	cmd := exec.CommandContext(ctx, name, args...)
	var outBuf, errBuf strings.Builder
	cmd.Stdout = &outBuf
	cmd.Stderr = &errBuf
	// A probe that emits nothing must still be reaped once the deadline
	// fires, so cap the post-cancel wait.
	cmd.WaitDelay = time.Second
	err := cmd.Run()
	return outBuf.String(), errBuf.String(), err == nil
}

// vmafHelp returns the vmaf --help output (stdout and stderr joined). Any
// error degrades to an empty string so probe logic reads "binary doesn't
// support GPU backends" instead of failing the run.
func vmafHelp(ctx context.Context, opts Options) string {
	bin := opts.vmafBin()
	if !strings.Contains(bin, "/") {
		if _, err := opts.lookPath()(bin); err != nil {
			return ""
		}
	}
	out, errOut, _ := opts.runner()(ctx, bin, "--help")
	return out + "\n" + errOut
}

// ParseSupportedBackends extracts the backends the vmaf binary advertises in
// its --help text. The fork's CLI prints a line like
//
//	--backend $name:   exclusive backend selector — auto|cpu|cuda|sycl|hip.
//
// so each backend token is matched only when it is delimited by a leading '|'
// and a trailing '|', '.', newline or space. That avoids false positives on
// substrings (e.g. the word "cuda" inside a prose comment). "cpu" is always
// present — every libvmaf build has a CPU path, even if the help line is
// missing entirely.
func ParseSupportedBackends(helpText string) map[string]bool {
	found := map[string]bool{"cpu": true}
	for _, backend := range AllBackends() {
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

// probeCUDA reports whether a CUDA device is reachable, via `nvidia-smi -L`.
func probeCUDA(ctx context.Context, opts Options) bool {
	if _, err := opts.lookPath()("nvidia-smi"); err != nil {
		return false
	}
	out, _, ok := opts.runner()(ctx, "nvidia-smi", "-L")
	return ok && strings.Contains(out, "GPU")
}

// probeSYCL reports whether a SYCL GPU device is reachable, via `sycl-ls`.
// sycl-ls prints one line per device prefixed with bracketed backend tokens:
// "[opencl:gpu]", "[ext_oneapi_level_zero:gpu]", ...
func probeSYCL(ctx context.Context, opts Options) bool {
	if _, err := opts.lookPath()("sycl-ls"); err != nil {
		return false
	}
	out, _, ok := opts.runner()(ctx, "sycl-ls")
	return ok && strings.Contains(out, "[") && strings.Contains(strings.ToLower(out), ":gpu")
}

// probeHIP reports whether an AMD ROCm/HIP GPU is reachable. rocminfo is
// tried first (it names the gfx target); rocm-smi is the fallback.
func probeHIP(ctx context.Context, opts Options) bool {
	look := opts.lookPath()
	run := opts.runner()

	if _, err := look("rocminfo"); err == nil {
		out, errOut, ok := run(ctx, "rocminfo")
		if ok && strings.Contains(strings.ToLower(out+"\n"+errOut), "gfx") {
			return true
		}
	}

	if _, err := look("rocm-smi"); err != nil {
		return false
	}
	out, errOut, ok := run(ctx, "rocm-smi", "--showproductname")
	if !ok {
		return false
	}
	lower := strings.ToLower(out + "\n" + errOut)
	return strings.Contains(lower, "gpu") || strings.Contains(lower, "card series")
}

// Detect returns the backends usable on this host, in AllBackends order.
func Detect(ctx context.Context, opts Options) []string {
	supported := ParseSupportedBackends(vmafHelp(ctx, opts))

	probes := map[string]bool{"cpu": true}
	if supported["cuda"] {
		probes["cuda"] = probeCUDA(ctx, opts)
	}
	if supported["sycl"] {
		probes["sycl"] = probeSYCL(ctx, opts)
	}
	if supported["hip"] {
		probes["hip"] = probeHIP(ctx, opts)
	}

	out := make([]string, 0, len(probes))
	for _, b := range AllBackends() {
		if supported[b] && probes[b] {
			out = append(out, b)
		}
	}
	return out
}

// Select picks a backend honouring the operator preference and the host
// capability.
//
//   - prefer "auto" walks opts.Fallbacks and returns the first entry present
//     in the available set. When nothing matches it returns "cpu", which is
//     universally available even if every probe failed.
//   - Any other prefer value (cpu / cuda / sycl / hip) is honoured strictly:
//     if it is not available, an *UnavailableError is returned. Select never
//     silently downgrades.
func Select(ctx context.Context, prefer string, opts Options) (string, error) {
	if prefer != "auto" {
		known := false
		for _, b := range AllBackends() {
			if b == prefer {
				known = true
				break
			}
		}
		if !known {
			return "", fmt.Errorf("unknown backend %q; expected one of: auto, %s",
				prefer, strings.Join(AllBackends(), ", "))
		}
	}

	available := opts.Available
	if available == nil {
		available = Detect(ctx, opts)
	}
	has := func(name string) bool {
		for _, a := range available {
			if a == name {
				return true
			}
		}
		return false
	}

	if prefer == "auto" {
		for _, candidate := range opts.fallbacks() {
			if has(candidate) {
				return candidate, nil
			}
		}
		// Last-ditch: cpu is universally available even if probes failed.
		return "cpu", nil
	}

	if has(prefer) {
		return prefer, nil
	}
	return "", &UnavailableError{Requested: prefer, Available: available}
}

// IsUnavailable reports whether err is (or wraps) an *UnavailableError.
func IsUnavailable(err error) bool {
	var target *UnavailableError
	return errors.As(err, &target)
}
