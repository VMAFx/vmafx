// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Command vmafx-ort-runner runs one ONNX Runtime forward pass through
// libvmaf's standalone DNN session API and prints the output tensor as
// JSON.
//
// It is the subprocess half of the Go ONNX seam (pkg/ai.Registry.Infer,
// ADR-0713 Stage 1). Binaries that must stay pure Go — vmafx-tune's
// predict / sidecar / auto `--model` paths — shell out to this runner
// instead of linking ONNX Runtime through cgo themselves. The runner is a
// cgo build over pkg/libvmaf, so it inherits libvmaf's model hardening
// (size cap, operator allowlist) and its execution-provider selection.
// ADR-1134 records why the runner lives in this repository and why the
// wire format is what it is.
//
// # Protocol
//
//	vmafx-ort-runner --model <path.onnx> --inputs '<JSON array of numbers>'
//
// The array is bound to the graph's single input as a float32 row vector
// of shape [1, N], where N is the array length; --input-name binds by
// graph input name instead of positionally. stdout receives the flattened
// output tensor as one JSON array of numbers followed by a newline, and
// nothing else. Diagnostics go to stderr.
//
// # Exit codes
//
//	0  success
//	1  the model could not be opened or executed
//	2  usage or protocol error (bad flags, malformed --inputs)
//	3  libvmaf was built without ONNX Runtime (meson -Denable_dnn)
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"strings"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// Exit codes are part of the runner's contract; see
// docs/usage/vmafx-ort-runner.md before changing them.
const (
	exitOK             = 0
	exitInference      = 1
	exitUsage          = 2
	exitDNNUnavailable = 3
)

// session is the slice of *libvmaf.DNNSession the runner drives. It is an
// interface so the protocol tests can substitute a fake and run on a
// libvmaf built without ONNX Runtime.
type session interface {
	Predict(ctx context.Context, inputName string, x []float32, rows, cols int) ([]float32, error)
	Close()
}

// openFunc opens a session for the model at path.
type openFunc func(path string) (session, error)

// openLibvmaf is the production openFunc.
func openLibvmaf(path string) (session, error) {
	sess, err := libvmaf.OpenDNNSession(path)
	if err != nil {
		return nil, err
	}
	return sess, nil
}

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr, openLibvmaf))
}

// run is main without the process boundary: it parses args, performs the
// forward pass, writes the result to stdout and returns the exit code.
func run(args []string, stdout, stderr io.Writer, open openFunc) int {
	fs := flag.NewFlagSet("vmafx-ort-runner", flag.ContinueOnError)
	fs.SetOutput(stderr)
	model := fs.String("model", "", "path to the .onnx model (required)")
	inputsJSON := fs.String("inputs", "",
		"JSON array of numbers, bound to the graph input as a [1, N] float32 row vector (required)")
	inputName := fs.String("input-name", "",
		"graph input name to bind --inputs to (default: positional, the first input)")
	if err := fs.Parse(args); err != nil {
		// flag has already written the diagnostic (or, for -h, the usage).
		if errors.Is(err, flag.ErrHelp) {
			return exitOK
		}
		return exitUsage
	}
	if fs.NArg() != 0 {
		fmt.Fprintf(stderr, "vmafx-ort-runner: unexpected positional argument %q\n", fs.Arg(0))
		return exitUsage
	}
	if *model == "" {
		fmt.Fprintln(stderr, "vmafx-ort-runner: --model is required")
		return exitUsage
	}
	inputs, err := parseInputs(*inputsJSON)
	if err != nil {
		fmt.Fprintf(stderr, "vmafx-ort-runner: --inputs: %v\n", err)
		return exitUsage
	}

	outputs, err := infer(open, *model, *inputName, inputs)
	if err != nil {
		fmt.Fprintf(stderr, "vmafx-ort-runner: %v\n", err)
		if errors.Is(err, libvmaf.ErrDNNUnavailable) {
			return exitDNNUnavailable
		}
		return exitInference
	}
	if err := writeOutputs(stdout, outputs); err != nil {
		fmt.Fprintf(stderr, "vmafx-ort-runner: %v\n", err)
		return exitInference
	}
	return exitOK
}

// parseInputs decodes the --inputs JSON array into float32 values.
//
// The value must be one JSON array with at least one number, and every
// number must be representable as a float32 — that is what the ONNX graph
// consumes, and a silent overflow to ±Inf would feed the model garbage
// without any error. JSON cannot carry NaN or ±Inf, so those need no check.
func parseInputs(raw string) ([]float32, error) {
	if strings.TrimSpace(raw) == "" {
		return nil, errors.New("required (a JSON array of numbers)")
	}
	var vals []float64
	if err := json.Unmarshal([]byte(raw), &vals); err != nil {
		return nil, fmt.Errorf("not a JSON array of numbers: %w", err)
	}
	if len(vals) == 0 {
		return nil, errors.New("array is empty")
	}
	out := make([]float32, len(vals))
	for i, v := range vals {
		if math.Abs(v) > math.MaxFloat32 {
			return nil, fmt.Errorf("element %d (%g) does not fit in float32", i, v)
		}
		out[i] = float32(v)
	}
	return out, nil
}

// infer opens the model and runs the single [1, N] forward pass.
func infer(open openFunc, modelPath, inputName string, inputs []float32) ([]float32, error) {
	sess, err := open(modelPath)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", modelPath, err)
	}
	defer sess.Close()
	out, err := sess.Predict(context.Background(), inputName, inputs, 1, len(inputs))
	if err != nil {
		return nil, fmt.Errorf("run %s: %w", modelPath, err)
	}
	return out, nil
}

// writeOutputs prints the flattened output tensor as one JSON array line.
//
// Values are widened to float64 before encoding — the same widening the
// Python predictor performs with float(np.float32) — so encoding/json
// prints the shortest decimal that round-trips the widened value and the
// reader recovers the float32 result exactly. A NaN or ±Inf output is
// refused here rather than reaching the consumer as an unparseable line.
func writeOutputs(w io.Writer, outputs []float32) error {
	widened := make([]float64, len(outputs))
	for i, v := range outputs {
		wide := float64(v)
		if math.IsNaN(wide) || math.IsInf(wide, 0) {
			return fmt.Errorf("output element %d is %v; refusing to emit a non-finite result", i, v)
		}
		widened[i] = wide
	}
	if err := json.NewEncoder(w).Encode(widened); err != nil {
		return fmt.Errorf("encode outputs: %w", err)
	}
	return nil
}
