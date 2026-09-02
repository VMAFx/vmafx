// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// predictorModel is the shipped per-shot predictor the runner is smoke-tested
// against; its graph has one input named "input" of shape [1, 14] and one
// output "vmaf" of shape [1, 1].
const predictorModel = "../../model/predictor_libx264.onnx"

// predictorInputs is a deliberately unrealistic feature row: the shipped
// predictor saturates at 100.0 for ordinary shots, and a saturated output
// cannot tell a correct forward pass from a wrong one.
var predictorInputs = []float64{51, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 16, 16}

// predictorReference is what onnxruntime 1.29.0's CPU execution provider
// returns for predictorInputs, captured in the dev container with:
//
//	python -c 'import onnxruntime as ort, numpy as np; \
//	  s = ort.InferenceSession("model/predictor_libx264.onnx", providers=["CPUExecutionProvider"]); \
//	  print(repr(float(s.run(None, {"input": np.array([[51,1,1,1,1,0,0,0,0,0,1,1,16,16]], dtype=np.float32)})[0].ravel()[0])))'
//
// The tolerance covers kernel-selection ULP noise between ORT builds and
// CPU ISAs while still rejecting the saturated 100.0 a mis-bound tensor
// produces.
const (
	predictorReference = 66.13961791992188
	predictorTolerance = 1e-2
)

// fakeSession records the call it receives and returns canned outputs.
type fakeSession struct {
	inputName  string
	x          []float32
	rows, cols int
	out        []float32
	err        error
	closed     bool
}

func (f *fakeSession) Predict(_ context.Context, inputName string, x []float32, rows, cols int) ([]float32, error) {
	f.inputName, f.x, f.rows, f.cols = inputName, x, rows, cols
	return f.out, f.err
}

func (f *fakeSession) Close() { f.closed = true }

// fakeOpen returns an openFunc handing out sess (or openErr) and records the
// path it was asked to open in *opened.
func fakeOpen(sess *fakeSession, openErr error, opened *string) openFunc {
	return func(path string) (session, error) {
		*opened = path
		if openErr != nil {
			return nil, openErr
		}
		return sess, nil
	}
}

func TestRun_ProtocolRoundTrip(t *testing.T) {
	t.Parallel()
	sess := &fakeSession{out: []float32{0.5, 66.5}}
	var opened string
	var stdout, stderr bytes.Buffer

	code := run([]string{"--model", "m.onnx", "--inputs", "[1, 2.5, -3]"},
		&stdout, &stderr, fakeOpen(sess, nil, &opened))

	if code != exitOK {
		t.Fatalf("exit %d, want %d; stderr=%q", code, exitOK, stderr.String())
	}
	if got, want := stdout.String(), "[0.5,66.5]\n"; got != want {
		t.Errorf("stdout = %q, want %q", got, want)
	}
	if stderr.Len() != 0 {
		t.Errorf("stderr = %q, want empty on success", stderr.String())
	}
	if opened != "m.onnx" {
		t.Errorf("opened %q, want m.onnx", opened)
	}
	if sess.inputName != "" {
		t.Errorf("input name %q, want positional (empty)", sess.inputName)
	}
	if sess.rows != 1 || sess.cols != 3 {
		t.Errorf("shape [%d, %d], want [1, 3]", sess.rows, sess.cols)
	}
	if want := []float32{1, 2.5, -3}; len(sess.x) != len(want) || sess.x[0] != want[0] || sess.x[1] != want[1] || sess.x[2] != want[2] {
		t.Errorf("inputs %v, want %v", sess.x, want)
	}
	if !sess.closed {
		t.Error("session was not closed")
	}
}

func TestRun_InputNameIsForwarded(t *testing.T) {
	t.Parallel()
	sess := &fakeSession{out: []float32{1}}
	var opened string
	var stdout, stderr bytes.Buffer
	code := run([]string{"--model", "m.onnx", "--inputs", "[1]", "--input-name", "features"},
		&stdout, &stderr, fakeOpen(sess, nil, &opened))
	if code != exitOK {
		t.Fatalf("exit %d; stderr=%q", code, stderr.String())
	}
	if sess.inputName != "features" {
		t.Errorf("input name %q, want features", sess.inputName)
	}
}

func TestRun_EmptyOutputIsAnEmptyArray(t *testing.T) {
	t.Parallel()
	sess := &fakeSession{out: []float32{}}
	var opened string
	var stdout, stderr bytes.Buffer
	if code := run([]string{"--model", "m.onnx", "--inputs", "[1]"}, &stdout, &stderr, fakeOpen(sess, nil, &opened)); code != exitOK {
		t.Fatalf("exit %d; stderr=%q", code, stderr.String())
	}
	// pkg/ai unmarshals into []float64 and treats len 0 as "no outputs";
	// "null" would decode to the same thing, but "[]" is the documented form.
	if got := stdout.String(); got != "[]\n" {
		t.Errorf("stdout = %q, want %q", got, "[]\n")
	}
}

// TestRun_UsageErrors pins exit 2 for every malformed invocation, with
// nothing on stdout and the session never opened — pkg/ai must be able to
// tell "you called me wrong" from "inference failed".
func TestRun_UsageErrors(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		args []string
	}{
		{"no arguments", nil},
		{"missing inputs", []string{"--model", "m.onnx"}},
		{"missing model", []string{"--inputs", "[1]"}},
		{"inputs is an object", []string{"--model", "m.onnx", "--inputs", "{}"}},
		{"inputs is a string", []string{"--model", "m.onnx", "--inputs", `"1"`}},
		{"inputs is null", []string{"--model", "m.onnx", "--inputs", "null"}},
		{"inputs is empty array", []string{"--model", "m.onnx", "--inputs", "[]"}},
		{"inputs has trailing garbage", []string{"--model", "m.onnx", "--inputs", "[1] x"}},
		{"inputs has non-number", []string{"--model", "m.onnx", "--inputs", `[1, "2"]`}},
		{"inputs overflow float32", []string{"--model", "m.onnx", "--inputs", "[1e39]"}},
		{"positional argument", []string{"--model", "m.onnx", "--inputs", "[1]", "extra"}},
		{"unknown flag", []string{"--model", "m.onnx", "--inputs", "[1]", "--bogus"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			var opened string
			var stdout, stderr bytes.Buffer
			code := run(tc.args, &stdout, &stderr, fakeOpen(&fakeSession{out: []float32{1}}, nil, &opened))
			if code != exitUsage {
				t.Errorf("exit %d, want %d", code, exitUsage)
			}
			if stdout.Len() != 0 {
				t.Errorf("stdout = %q, want empty", stdout.String())
			}
			if stderr.Len() == 0 {
				t.Error("stderr is empty; a usage error must explain itself")
			}
			if opened != "" {
				t.Errorf("session opened for %q before validation finished", opened)
			}
		})
	}
}

func TestRun_HelpExitsZero(t *testing.T) {
	t.Parallel()
	var opened string
	var stdout, stderr bytes.Buffer
	code := run([]string{"-h"}, &stdout, &stderr, fakeOpen(&fakeSession{}, nil, &opened))
	if code != exitOK {
		t.Errorf("exit %d, want %d", code, exitOK)
	}
	// The flag package prints single-dash names ("-model"); both forms parse.
	if !strings.Contains(stderr.String(), "-model") {
		t.Errorf("usage text %q does not mention -model", stderr.String())
	}
	if stdout.Len() != 0 {
		t.Errorf("stdout = %q, want empty (usage goes to stderr)", stdout.String())
	}
}

func TestRun_DNNUnavailableIsExit3(t *testing.T) {
	t.Parallel()
	var opened string
	var stdout, stderr bytes.Buffer
	code := run([]string{"--model", "m.onnx", "--inputs", "[1]"},
		&stdout, &stderr, fakeOpen(nil, libvmaf.ErrDNNUnavailable, &opened))
	if code != exitDNNUnavailable {
		t.Errorf("exit %d, want %d", code, exitDNNUnavailable)
	}
	// The operator fix is a rebuild flag; the message must name it.
	if !strings.Contains(stderr.String(), "enable_dnn") {
		t.Errorf("stderr %q does not mention enable_dnn", stderr.String())
	}
	if stdout.Len() != 0 {
		t.Errorf("stdout = %q, want empty", stdout.String())
	}
}

func TestRun_InferenceFailureIsExit1(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		sess *fakeSession
		open error
	}{
		{"open fails", nil, errors.New("model not found")},
		{"predict fails", &fakeSession{err: errors.New("ORT failed")}, nil},
		{"NaN output", &fakeSession{out: []float32{float32(math.NaN())}}, nil},
		{"Inf output", &fakeSession{out: []float32{float32(math.Inf(1))}}, nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			var opened string
			var stdout, stderr bytes.Buffer
			code := run([]string{"--model", "m.onnx", "--inputs", "[1]"}, &stdout, &stderr, fakeOpen(tc.sess, tc.open, &opened))
			if code != exitInference {
				t.Errorf("exit %d, want %d", code, exitInference)
			}
			if stdout.Len() != 0 {
				t.Errorf("stdout = %q, want empty on failure", stdout.String())
			}
			if stderr.Len() == 0 {
				t.Error("stderr is empty; a failure must explain itself")
			}
			if tc.sess != nil && !tc.sess.closed {
				t.Error("session leaked on the failure path")
			}
		})
	}
}

// requirePredictor returns the shipped predictor model path, skipping when
// the tree or the libvmaf build cannot run it.
func requirePredictor(t *testing.T) string {
	t.Helper()
	if !libvmaf.DNNAvailable() {
		t.Skip("libvmaf built without ONNX Runtime; rebuild with -Denable_dnn=enabled")
	}
	if _, err := os.Stat(predictorModel); err != nil {
		t.Skipf("predictor model absent: %v", err)
	}
	abs, err := filepath.Abs(predictorModel)
	if err != nil {
		t.Fatalf("abs: %v", err)
	}
	return abs
}

func marshalInputs(t *testing.T) string {
	t.Helper()
	b, err := json.Marshal(predictorInputs)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return string(b)
}

func decodeSingle(t *testing.T, stdout string) float64 {
	t.Helper()
	var got []float64
	if err := json.Unmarshal([]byte(stdout), &got); err != nil {
		t.Fatalf("stdout %q is not a JSON array: %v", stdout, err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d outputs %v, want 1", len(got), got)
	}
	return got[0]
}

// TestRun_PredictorModel is the real thing: the shipped predictor through
// libvmaf's ONNX Runtime session, compared against onnxruntime's own answer.
func TestRun_PredictorModel(t *testing.T) {
	model := requirePredictor(t)
	var stdout, stderr bytes.Buffer
	code := run([]string{"--model", model, "--inputs", marshalInputs(t)}, &stdout, &stderr, openLibvmaf)
	if code != exitOK {
		t.Fatalf("exit %d; stderr=%q", code, stderr.String())
	}
	if !strings.HasSuffix(stdout.String(), "\n") {
		t.Errorf("stdout %q does not end with a newline", stdout.String())
	}
	got := decodeSingle(t, stdout.String())
	if math.Abs(got-predictorReference) > predictorTolerance {
		t.Errorf("predictor output %v, want %v ± %v", got, predictorReference, predictorTolerance)
	}
}

// TestRun_PredictorModelByName pins that binding by the graph's input name
// yields the same result as positional binding, and that a wrong name is an
// inference error rather than a silent positional fallback.
func TestRun_PredictorModelByName(t *testing.T) {
	model := requirePredictor(t)
	var stdout, stderr bytes.Buffer
	code := run([]string{"--model", model, "--inputs", marshalInputs(t), "--input-name", "input"},
		&stdout, &stderr, openLibvmaf)
	if code != exitOK {
		t.Fatalf("exit %d; stderr=%q", code, stderr.String())
	}
	if got := decodeSingle(t, stdout.String()); math.Abs(got-predictorReference) > predictorTolerance {
		t.Errorf("by-name output %v, want %v ± %v", got, predictorReference, predictorTolerance)
	}

	stdout.Reset()
	stderr.Reset()
	code = run([]string{"--model", model, "--inputs", marshalInputs(t), "--input-name", "no-such-input"},
		&stdout, &stderr, openLibvmaf)
	if code != exitInference {
		t.Errorf("wrong input name: exit %d, want %d; stdout=%q", code, exitInference, stdout.String())
	}
}

// TestRun_MissingModelFile pins the exit code for an absent model on both
// libvmaf builds: exit 1 with ORT (the stat fails), exit 3 without it (the
// -ENOSYS stub answers before any file is touched).
func TestRun_MissingModelFile(t *testing.T) {
	t.Parallel()
	var stdout, stderr bytes.Buffer
	code := run([]string{"--model", filepath.Join(t.TempDir(), "absent.onnx"), "--inputs", "[1]"},
		&stdout, &stderr, openLibvmaf)
	want := exitInference
	if !libvmaf.DNNAvailable() {
		want = exitDNNUnavailable
	}
	if code != want {
		t.Errorf("exit %d, want %d; stderr=%q", code, want, stderr.String())
	}
	if stdout.Len() != 0 {
		t.Errorf("stdout = %q, want empty", stdout.String())
	}
}
