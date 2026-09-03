// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/ai/infer_runner_test.go — the subprocess contract between
// Registry.Infer and vmafx-ort-runner (ADR-1134).
//
// The fake-runner tests pin the wire format from this side without needing
// an ONNX Runtime build: what argv the runner receives, how its stdout is
// parsed, and how its failures are reported. TestInfer_RealRunner then runs
// the real binary when it is on PATH (the Go CI job and the dev container
// put it there) against the shipped predictor model.

package ai

import (
	"context"
	"errors"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// installFakeRunner puts an executable vmafx-ort-runner shell script alone
// on PATH for the duration of the test and returns the file its "$@" is
// recorded in.
func installFakeRunner(t *testing.T, body string) string {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake runner is a POSIX shell script")
	}
	dir := t.TempDir()
	argsFile := filepath.Join(dir, "args.txt")
	script := "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" + argsFile + "\"\n" + body + "\n"
	if err := os.WriteFile(filepath.Join(dir, "vmafx-ort-runner"), []byte(script), 0o700); err != nil {
		t.Fatalf("write fake runner: %v", err)
	}
	t.Setenv("PATH", dir)
	return argsFile
}

func readArgs(t *testing.T, argsFile string) []string {
	t.Helper()
	data, err := os.ReadFile(argsFile)
	if err != nil {
		t.Fatalf("runner was not invoked (no args file): %v", err)
	}
	return strings.Split(strings.TrimRight(string(data), "\n"), "\n")
}

// TestInfer_FakeRunnerProtocol pins argv and stdout parsing: the runner gets
// `--model <resolved path> --inputs <JSON array>` and its one-line JSON
// array comes back as []float64.
func TestInfer_FakeRunnerProtocol(t *testing.T) {
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")
	argsFile := installFakeRunner(t, "echo '[1.5, 2.5]'")

	r := NewRegistry(dir)
	out, err := r.Infer(context.Background(), "nr_metric_v1", []float64{0.5, 0.25})
	if err != nil {
		t.Fatalf("Infer: %v", err)
	}
	if len(out) != 2 || out[0] != 1.5 || out[1] != 2.5 {
		t.Errorf("outputs %v, want [1.5 2.5]", out)
	}

	args := readArgs(t, argsFile)
	want := []string{"--model", filepath.Join(dir, "nr_metric_v1.onnx"), "--inputs", "[0.5,0.25]"}
	if len(args) != len(want) {
		t.Fatalf("argv %q, want %q", args, want)
	}
	for i := range want {
		if args[i] != want[i] {
			t.Errorf("argv[%d] = %q, want %q", i, args[i], want[i])
		}
	}
}

// TestInfer_FakeRunnerFailureCarriesStderr pins that a failing runner's
// explanation reaches the caller: exit 3 is what cmd/vmafx-ort-runner
// returns on a libvmaf built without ONNX Runtime, and "exit status 3"
// alone would send an operator to the wrong fix.
func TestInfer_FakeRunnerFailureCarriesStderr(t *testing.T) {
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")
	installFakeRunner(t, "echo 'libvmaf was built without DNN support' >&2; exit 3")

	r := NewRegistry(dir)
	_, err := r.Infer(context.Background(), "nr_metric_v1", []float64{1})
	if err == nil {
		t.Fatal("expected an error from a runner exiting 3")
	}
	if errors.Is(err, ErrORTRunnerNotFound) {
		t.Errorf("error %v claims the runner is absent, but it ran and failed", err)
	}
	for _, want := range []string{"exit status 3", "without DNN support"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error %q does not contain %q", err, want)
		}
	}
}

// TestInfer_FakeRunnerGarbageOutput pins that a non-JSON stdout is an error
// that quotes the raw output rather than a zero-length "success".
func TestInfer_FakeRunnerGarbageOutput(t *testing.T) {
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")
	installFakeRunner(t, "echo 'not json'")

	r := NewRegistry(dir)
	_, err := r.Infer(context.Background(), "nr_metric_v1", []float64{1})
	if err == nil {
		t.Fatal("expected a parse error")
	}
	if !strings.Contains(err.Error(), "not json") {
		t.Errorf("error %q does not quote the raw output", err)
	}
}

// TestInfer_RealRunner drives the real vmafx-ort-runner when it is on PATH
// and the shipped predictor model is in the tree. The expected value and
// tolerance are shared with cmd/vmafx-ort-runner's own smoke test; see the
// predictorReference comment there for how it was captured.
func TestInfer_RealRunner(t *testing.T) {
	if _, err := exec.LookPath("vmafx-ort-runner"); err != nil {
		t.Skip("vmafx-ort-runner not on PATH; build it with `make go-ort-runner`")
	}
	modelDir, err := filepath.Abs("../../model")
	if err != nil {
		t.Fatalf("abs: %v", err)
	}
	if _, err := os.Stat(filepath.Join(modelDir, "predictor_libx264.onnx")); err != nil {
		t.Skipf("predictor model absent: %v", err)
	}

	r := NewRegistry(modelDir)
	out, err := r.Infer(context.Background(), "predictor_libx264",
		[]float64{51, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 16, 16})
	if err != nil {
		if strings.Contains(err.Error(), "exit status 3") {
			t.Skipf("runner on PATH was built against a libvmaf without ONNX Runtime: %v", err)
		}
		t.Fatalf("Infer through the real runner: %v", err)
	}
	if len(out) != 1 {
		t.Fatalf("outputs %v, want exactly one", out)
	}
	const want, tol = 66.13961791992188, 1e-2
	if math.Abs(out[0]-want) > tol {
		t.Errorf("predictor output %v, want %v ± %v", out[0], want, tol)
	}
}
