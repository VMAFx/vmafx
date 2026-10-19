// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package scorebackend picks the libvmaf scoring backend for a run; it is the
// Go twin of the backend-selection half of
// tools/vmaf-tune/src/vmaftune/score_backend.py.
//
// It answers one question: which libvmaf scoring backend (cpu / cuda / sycl /
// hip / metal) should this run use? Which backends are usable is not decided
// here: `vmaf --list-backends` (ADR-1874) reports, per backend, whether it was
// compiled into the binary and whether its state initialises on this host, and
// Detect reads that report. Both selectors replay the shared cases in
// testdata/score_backend_selection.json.
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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"slices"
	"strings"
	"time"
)

// AllBackends lists the backends `vmaf --list-backends` reports and the vmaf
// CLI accepts via --backend NAME, in the report's order.
func AllBackends() []string { return []string{"cpu", "cuda", "sycl", "hip", "metal"} }

// DefaultFallbacks is the fallback chain for prefer="auto": native GPU
// backends in the order the CLI lists them, cpu as the always-available floor.
func DefaultFallbacks() []string { return []string{"cuda", "sycl", "hip", "metal", "cpu"} }

// probeTimeout bounds one `vmaf --list-backends` run, which initialises every
// compiled GPU backend once. Mirrors LIST_BACKENDS_TIMEOUT_S in Python.
const probeTimeout = 60 * time.Second

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
			"Run `vmaf --list-backends`: the backend must be compiled into the vmaf "+
			"binary and initialise on this host (runtime/driver installed).",
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
	// Fallbacks overrides the prefer="auto" chain. nil selects
	// DefaultFallbacks; an explicit empty slice goes directly to the CPU floor.
	Fallbacks []string
	// Available short-circuits host detection with a literal list. Used by
	// tests to keep the unit boundary tight; nil runs Detect.
	Available []string
	// Run overrides subprocess execution. nil selects execRunner.
	Run Runner
	// LookPath overrides binary discovery. nil selects exec.LookPath.
	LookPath func(string) (string, error)
	// Warnings receives the warning Detect writes when the backend report
	// is unavailable and only cpu is usable. nil selects os.Stderr.
	Warnings io.Writer
}

func (o Options) vmafBin() string {
	if o.VMAFBin == "" {
		return "vmaf"
	}
	return o.VMAFBin
}

func (o Options) fallbacks() []string {
	if o.Fallbacks == nil {
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

func (o Options) warnings() io.Writer {
	if o.Warnings == nil {
		return os.Stderr
	}
	return o.Warnings
}

// execRunner is the production Runner: it executes name with args under a
// probeTimeout deadline and reports (stdout, stderr, exit==0).
func execRunner(ctx context.Context, name string, args ...string) (string, string, bool) {
	ctx, cancel := context.WithTimeout(ctx, probeTimeout)
	defer cancel()
	// #nosec G204 -- name is the operator-configured vmaf binary and args
	// the fixed --list-backends literal. ctx enforces probeTimeout.
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

// BackendStatus is one row of the `vmaf --list-backends` report.
type BackendStatus struct {
	// Name is the --backend value.
	Name string `json:"name"`
	// Compiled is true when the backend is built into the binary.
	Compiled bool `json:"compiled"`
	// Usable is true when the backend's state initialised on this host.
	Usable *bool `json:"usable"`
	// InitStatus is the negative errno of a compiled backend that did not
	// initialise; zero otherwise.
	InitStatus int `json:"init_status,omitempty"`
}

// ParseReport reads a `vmaf --list-backends` document into its rows, keyed by
// backend name. Names this package does not know are kept; selection ignores
// them.
func ParseReport(text string) (map[string]BackendStatus, error) {
	var doc struct {
		Backends []BackendStatus `json:"backends"`
	}
	if err := json.Unmarshal([]byte(text), &doc); err != nil {
		return nil, fmt.Errorf("not a backend report: %w", err)
	}
	report := make(map[string]BackendStatus, len(doc.Backends))
	for _, row := range doc.Backends {
		if row.Name == "" || row.Usable == nil {
			return nil, fmt.Errorf("malformed backend row %+v", row)
		}
		report[row.Name] = row
	}
	if len(report) == 0 {
		return nil, errors.New("the backend report lists no backends")
	}
	return report, nil
}

// UsableBackends returns the usable backends of a report in AllBackends
// order; cpu is always included.
func UsableBackends(report map[string]BackendStatus) []string {
	out := []string{"cpu"}
	for _, name := range AllBackends()[1:] {
		if row, ok := report[name]; ok && row.Usable != nil && *row.Usable {
			out = append(out, name)
		}
	}
	return out
}

// Report runs `vmaf --list-backends` and returns its rows by backend name.
// The error names the binary and the reason when it is missing, fails (a
// vmaf older than ADR-1874 rejects the option) or prints no report.
func Report(ctx context.Context, opts Options) (map[string]BackendStatus, error) {
	bin := opts.vmafBin()
	if !strings.Contains(bin, "/") {
		if _, err := opts.lookPath()(bin); err != nil {
			return nil, fmt.Errorf("%q is not on PATH: %w", bin, err)
		}
	}
	out, errOut, ok := opts.runner()(ctx, bin, "--list-backends")
	if !ok {
		return nil, fmt.Errorf("%s --list-backends failed (a vmaf older than ADR-1874 "+
			"does not have the option): %s", bin, strings.TrimSpace(errOut))
	}
	return ParseReport(out)
}

// ParseSupportedBackends extracts the backends named in `vmaf --help`.
//
// Deprecated: the help text names every backend whatever the build, so the
// result says nothing about the binary. Detect reads `vmaf --list-backends`
// instead; use Report and UsableBackends.
func ParseSupportedBackends(helpText string) map[string]bool {
	found := map[string]bool{"cpu": true}
	for _, backend := range AllBackends() {
		for _, sep := range []string{"|", ".", "\n", " "} {
			if strings.Contains(helpText, "|"+backend+sep) {
				found[backend] = true
				break
			}
		}
	}
	return found
}

// Detect returns the backends usable on this host, in AllBackends order, as
// `vmaf --list-backends` reports them. When the report is unavailable the
// result is cpu alone and a warning naming the reason goes to opts.Warnings.
func Detect(ctx context.Context, opts Options) []string {
	report, err := Report(ctx, opts)
	if err != nil {
		log.New(opts.warnings(), "scorebackend: ", 0).Printf(
			"cannot read the vmaf backend report (%v); only cpu is usable", err)
		return []string{"cpu"}
	}
	return UsableBackends(report)
}

// Select picks a backend honouring the operator preference and the host
// capability.
//
//   - prefer "auto" walks opts.Fallbacks and returns the first entry present
//     in the available set. When nothing matches it returns "cpu", which is
//     universally available even if every probe failed.
//   - Any other prefer value (cpu / cuda / sycl / hip / metal) is honoured
//     strictly: if it is not available, an *UnavailableError is returned.
//     Select never silently downgrades. cpu needs no report.
func Select(ctx context.Context, prefer string, opts Options) (string, error) {
	if prefer != "auto" {
		known := slices.Contains(AllBackends(), prefer)
		if !known {
			return "", fmt.Errorf("unknown backend %q; expected one of: auto, %s",
				prefer, strings.Join(AllBackends(), ", "))
		}
	}
	if prefer == "cpu" {
		return "cpu", nil
	}

	available := opts.Available
	if available == nil {
		available = Detect(ctx, opts)
	}
	has := func(name string) bool {
		return slices.Contains(available, name)
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
