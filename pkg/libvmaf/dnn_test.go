// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Tests for the libvmaf ONNX-session binding.
//
// libvmaf always exports the dnn.h symbols, but their behaviour depends
// on whether ONNX Runtime was found at build time: with ORT they run a
// real session, without it every entry point returns -ENOSYS. Both
// builds are legitimate, so the tests branch on DNNAvailable() rather
// than assuming one. The -ENOSYS branch is the one CI exercises, since
// the CI libvmaf build installs no onnxruntime.

//go:build cgo

package libvmaf

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// findModel locates a committed .onnx model, skipping the test if the
// tree does not have one.
func findModel(t *testing.T) string {
	t.Helper()
	for _, rel := range []string{
		"../../model/tiny/fr_regressor_v1.onnx",
		"../../model/tiny/dists_sq.onnx",
		"../../model/predictor_libx264.onnx",
	} {
		if _, err := os.Stat(rel); err == nil {
			abs, err := filepath.Abs(rel)
			if err != nil {
				t.Fatalf("abs(%s): %v", rel, err)
			}
			return abs
		}
	}
	t.Skip("no committed .onnx model found")
	return ""
}

func TestDNNAvailableIsConsistent(t *testing.T) {
	// Whatever it reports must be stable across calls; a flapping value
	// would make the eval tool nondeterministic.
	first := DNNAvailable()
	for range 5 {
		if DNNAvailable() != first {
			t.Fatal("DNNAvailable() is not stable across calls")
		}
	}
	t.Logf("libvmaf DNN support: %v", first)
}

// TestOpenDNNSessionWithoutORT pins the graceful-degradation contract on
// a libvmaf built without ONNX Runtime.
func TestOpenDNNSessionWithoutORT(t *testing.T) {
	if DNNAvailable() {
		t.Skip("libvmaf has DNN support; -ENOSYS path not reachable")
	}
	model := findModel(t)
	sess, err := OpenDNNSession(model)
	if err == nil {
		sess.Close()
		t.Fatal("expected ErrDNNUnavailable on a build without ONNX Runtime")
	}
	if !errors.Is(err, ErrDNNUnavailable) {
		t.Fatalf("error = %v, want ErrDNNUnavailable", err)
	}
	// The message must tell an operator how to fix it.
	if !strings.Contains(err.Error(), "enable_dnn") {
		t.Errorf("error %q does not mention the build flag", err)
	}
}

// TestDNNSessionRoundTrip runs a real inference when ORT is present.
func TestDNNSessionRoundTrip(t *testing.T) {
	if !DNNAvailable() {
		t.Skip("libvmaf built without ONNX Runtime")
	}
	model := findModel(t)
	sess, err := OpenDNNSession(model)
	if err != nil {
		t.Skipf("model %s did not open (shape/allowlist): %v", model, err)
	}
	defer sess.Close()

	if ep := sess.AttachedEP(); ep == "" {
		t.Error("AttachedEP() is empty on an open session")
	} else {
		t.Logf("attached execution provider: %s", ep)
	}
}

func TestOpenDNNSessionMissingFile(t *testing.T) {
	_, err := OpenDNNSession(filepath.Join(t.TempDir(), "absent.onnx"))
	if err == nil {
		t.Fatal("expected an error for a missing model file")
	}
	// Without ORT the stub short-circuits to -ENOSYS before it can stat
	// the file, so both outcomes are correct depending on the build.
	if DNNAvailable() && errors.Is(err, ErrDNNUnavailable) {
		t.Error("ErrDNNUnavailable on a build that reports DNN support")
	}
}

// TestDNNSessionRunValidatesShape covers the argument checks, which run
// before any C call and so are testable on every build.
func TestDNNSessionRunValidatesShape(t *testing.T) {
	cases := []struct {
		name       string
		x          []float32
		rows, cols int
	}{
		{"zero rows", []float32{}, 0, 3},
		{"zero cols", []float32{}, 3, 0},
		{"negative rows", []float32{1}, -1, 1},
		{"buffer too small", []float32{1, 2}, 2, 3},
		{"buffer too large", make([]float32, 12), 2, 3},
	}
	// A zero-value session is closed; shape validation must reject these
	// before the closed-session check is reached.
	var sess DNNSession
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := sess.Predict(context.Background(), "features", tc.x, tc.rows, tc.cols)
			if err == nil {
				t.Fatal("expected a validation error")
			}
			if errors.Is(err, ErrDNNSessionClosed) {
				t.Errorf("shape error expected before the closed-session check, got %v", err)
			}
		})
	}
}

func TestDNNSessionClosedIsRejected(t *testing.T) {
	var sess DNNSession // zero value: no live handle
	_, err := sess.Predict(context.Background(), "features", []float32{1, 2, 3}, 1, 3)
	if !errors.Is(err, ErrDNNSessionClosed) {
		t.Fatalf("error = %v, want ErrDNNSessionClosed", err)
	}
}

// TestDNNSessionCloseIsIdempotent guards the double-close path — Compare
// closes every session it opens, and a stray extra Close must not crash.
func TestDNNSessionCloseIsIdempotent(t *testing.T) {
	var sess DNNSession
	sess.Close()
	sess.Close()
	if ep := sess.AttachedEP(); ep != "" {
		t.Errorf("AttachedEP() = %q on a closed session, want empty", ep)
	}
}

// TestDNNSessionPositionalBinding pins that an empty input name binds the
// tensor positionally (dnn.h: NULL descriptor name), which is how
// cmd/vmafx-ort-runner drives a graph whose input name it does not know
// (ADR-1134). The shipped predictor names its input "input", so by-name and
// positional runs must agree, and a wrong name must fail rather than fall
// back to positional silently.
func TestDNNSessionPositionalBinding(t *testing.T) {
	if !DNNAvailable() {
		t.Skip("libvmaf built without ONNX Runtime")
	}
	rel := "../../model/predictor_libx264.onnx"
	if _, err := os.Stat(rel); err != nil {
		t.Skipf("predictor model absent: %v", err)
	}
	model, err := filepath.Abs(rel)
	if err != nil {
		t.Fatalf("abs: %v", err)
	}
	sess, err := OpenDNNSession(model)
	if err != nil {
		t.Fatalf("OpenDNNSession(%s): %v", model, err)
	}
	defer sess.Close()

	x := []float32{51, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 16, 16}
	byPos, err := sess.Predict(context.Background(), "", x, 1, len(x))
	if err != nil {
		t.Fatalf("positional Predict: %v", err)
	}
	byName, err := sess.Predict(context.Background(), "input", x, 1, len(x))
	if err != nil {
		t.Fatalf("by-name Predict: %v", err)
	}
	if len(byPos) != 1 || len(byName) != 1 {
		t.Fatalf("outputs positional=%v by-name=%v, want one element each", byPos, byName)
	}
	if byPos[0] != byName[0] {
		t.Errorf("positional %v != by-name %v", byPos[0], byName[0])
	}
	// onnxruntime 1.29.0 CPU EP reference for this row (see
	// cmd/vmafx-ort-runner/main_test.go predictorReference).
	if got := float64(byPos[0]); got < 66.12 || got > 66.16 {
		t.Errorf("predictor output %v, want ≈ 66.1396", got)
	}
	if _, err := sess.Predict(context.Background(), "no-such-input", x, 1, len(x)); err == nil {
		t.Error("Predict with a wrong input name succeeded; expected an error, not a positional fallback")
	}
}

// TestDNNSessionRunCgoPointerRules is a regression test for a real
// panic: run() builds VmafDnnInput / VmafDnnOutput descriptors in Go
// memory that hold Go pointers (into x, shape and out) and passes their
// addresses to C. The cgo pointer rules forbid that unless the pointed-to
// arrays are pinned, so before the runtime.Pinner fix every call died
// with "cgo argument has Go pointer to unpinned Go pointer".
//
// The bug is unreachable through OpenDNNSession on a build without ONNX
// Runtime (the session never opens), so this drives run() against a dummy
// handle instead. cgocheck validates the arguments before the C function
// body executes, so the -ENOSYS stub is enough to exercise the rules.
func TestDNNSessionRunCgoPointerRules(t *testing.T) {
	if DNNAvailable() {
		t.Skip("dummy session handle is only safe against the -ENOSYS stub")
	}
	sess := newSessionWithDummyHandleForTest()
	defer sess.freeDummyHandleForTest()

	cases := []struct {
		name       string
		rows, cols int
	}{
		{"single row", 1, 6},
		{"many rows", 256, 6},
		{"single column", 8, 1},
		{"wide", 2, 64},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			x := make([]float32, tc.rows*tc.cols)
			for i := range x {
				x[i] = float32(i)
			}
			// Must not panic. On the stub build the C side returns
			// -ENOSYS, which maps to ErrDNNUnavailable.
			_, err := sess.Predict(context.Background(), "features", x, tc.rows, tc.cols)
			if !errors.Is(err, ErrDNNUnavailable) {
				t.Fatalf("error = %v, want ErrDNNUnavailable", err)
			}
		})
	}
}

func TestDNNErrMapping(t *testing.T) {
	cases := []struct {
		name    string
		rc      int
		wantErr error
		substr  string
	}{
		{"success is nil", 0, nil, ""},
		{"ENOSYS", -38, ErrDNNUnavailable, ""},
		{"E2BIG", -7, ErrDNNModelTooLarge, ""},
		{"ENOENT", -2, nil, "not found"},
		{"EINVAL", -22, nil, "invalid argument"},
		{"EIO", -5, nil, "ONNX Runtime failed"},
		{"ENOSPC", -28, nil, "too small"},
		{"unknown code", -999, nil, "returned -999"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := dnnErr("op", tc.rc)
			if tc.rc == 0 {
				if err != nil {
					t.Fatalf("rc=0 produced %v, want nil", err)
				}
				return
			}
			if err == nil {
				t.Fatal("expected an error")
			}
			if tc.wantErr != nil && !errors.Is(err, tc.wantErr) {
				t.Errorf("error = %v, want %v", err, tc.wantErr)
			}
			if tc.substr != "" && !strings.Contains(err.Error(), tc.substr) {
				t.Errorf("error %q does not contain %q", err, tc.substr)
			}
		})
	}
}
