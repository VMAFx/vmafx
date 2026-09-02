// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/ai/infer.go — Go ONNX Runtime inference layer for vmafx-node.
//
// # Stage 1 design
//
// The AI inference path on the node is intentionally thin for Stage 1.
// Rather than pulling in a full ONNX Runtime CGO binding (which requires
// pre-built libtensorrt / libnvonnxparser or the ORT shared lib at link
// time), this package provides:
//
//   - A Registry that maps model names to .onnx file paths resolved from
//     VMAFX_MODEL_DIR (default /usr/local/share/vmafx/model/).
//   - An Infer method that validates inputs and invokes the ORT subprocess
//     (vmafx-ort-runner) for Stage 1.  The subprocess approach is identical
//     to the libvmaf subprocess wrapper in pkg/libvmaf — it avoids CGO
//     build-time coupling on the ORT shared library.  A direct CGO path via
//     github.com/yalue/onnxruntime_go is the Stage 2 upgrade.
//
// The direct ORT CGO path is stubbed in InferDirect; it will be promoted
// to the main code path once the container image pins ORT 1.18+.
//
// ADR-0713: vmafx-node Go worker binary.

package ai

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"iter"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"
)

// DefaultModelDir is the filesystem location searched for ONNX model files
// when VMAFX_MODEL_DIR is not set.
const DefaultModelDir = "/usr/local/share/vmafx/model"

// inferTimeout bounds a single vmafx-ort-runner subprocess invocation when
// the caller's context carries no deadline of its own.  A wedged ORT runner
// (stuck device init, model deadlock) would otherwise hang the worker job.
const inferTimeout = 5 * time.Minute

// Registry manages the set of available ONNX models.
type Registry struct {
	modelDir string
}

// NewRegistry creates a Registry rooted at modelDir.
// Pass "" to use the VMAFX_MODEL_DIR environment variable (defaulting to
// DefaultModelDir).
func NewRegistry(modelDir string) *Registry {
	if modelDir == "" {
		if env := os.Getenv("VMAFX_MODEL_DIR"); env != "" {
			modelDir = env
		} else {
			modelDir = DefaultModelDir
		}
	}
	return &Registry{modelDir: modelDir}
}

// ModelPath resolves the filesystem path for a named ONNX model.
// Search order:
//  1. Exact file at modelName (if it is an absolute path that exists).
//  2. <modelDir>/<modelName>.onnx
//  3. <modelDir>/<modelName>  (if modelName already ends with .onnx)
func (r *Registry) ModelPath(modelName string) (string, error) {
	if filepath.IsAbs(modelName) {
		if _, err := os.Stat(modelName); err == nil {
			return modelName, nil
		}
	}
	for _, candidate := range []string{
		filepath.Join(r.modelDir, modelName+".onnx"),
		filepath.Join(r.modelDir, modelName),
	} {
		if _, err := os.Stat(candidate); err == nil {
			return candidate, nil
		}
	}
	return "", fmt.Errorf("ai: model %q not found in %s", modelName, r.modelDir)
}

// Infer runs ONNX Runtime inference on modelName with the given float64 inputs
// and returns the output tensor as a []float64 slice.
//
// Stage 1 implementation: shells out to vmafx-ort-runner for the actual ORT
// session, which avoids CGO build-time coupling on libtensorrt /
// libonnxruntime at the Go layer.
//
// The runner is built from this repository: cmd/vmafx-ort-runner is a cgo
// shim over pkg/libvmaf's ONNX session API, produced by `go build ./cmd/...`,
// `make go-ort-runner`, the dev container's go-build stage and the Go CI job
// (ADR-1134). Its wire format is exactly what this function sends —
// `--model <path> --inputs '<JSON array>'` in, one JSON array of numbers on
// stdout out — and the runner binds the array as a [1, N] float32 row vector
// on the graph's single input. Two things still make a fallback necessary:
// the runner is absent from PATH (ErrORTRunnerNotFound), or it is present but
// its libvmaf was built without ONNX Runtime (exit status 3, reported with
// the runner's stderr in the error).
//
// ctx bounds and cancels the subprocess: a cancelled context (job aborted,
// parent shutdown) or an elapsed deadline tears down vmafx-ort-runner via
// SIGKILL instead of leaving a wedged ORT session hanging the worker.  When
// ctx carries no deadline of its own, inferTimeout is applied as an upper
// bound, mirroring pkg/encoder and pkg/bisect.
//
// When vmafx-ort-runner is not found on PATH, returns ErrORTRunnerNotFound
// so callers can fall back gracefully.
func (r *Registry) Infer(ctx context.Context, modelName string, inputs []float64) ([]float64, error) {
	modelPath, err := r.ModelPath(modelName)
	if err != nil {
		return nil, err
	}

	runnerPath, lookErr := exec.LookPath("vmafx-ort-runner")
	if lookErr != nil {
		return nil, fmt.Errorf("%w: %v", ErrORTRunnerNotFound, lookErr)
	}

	// Encode inputs as a JSON array for the subprocess.
	inputJSON, err := json.Marshal(inputs)
	if err != nil {
		return nil, fmt.Errorf("ai: marshal inputs: %w", err)
	}

	// Apply inferTimeout only when the caller's context has no deadline of
	// its own, so an explicit caller deadline always wins.
	cancel := func() {}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		ctx, cancel = context.WithTimeout(ctx, inferTimeout)
	}
	defer cancel()

	// #nosec G204 -- runnerPath comes from exec.LookPath("vmafx-ort-runner"),
	// modelPath is resolved via r.ModelPath (registry-controlled), inputJSON
	// is the JSON-marshalled []float64 inputs.  ctx enforces the timeout.
	cmd := exec.CommandContext(ctx, runnerPath,
		"--model", modelPath,
		"--inputs", string(inputJSON),
	)
	// Short post-cancel grace so cmd.Output() unblocks even if the runner
	// holds inherited pipe descriptors open after the SIGKILL.
	cmd.WaitDelay = 2 * time.Second
	out, runErr := cmd.Output()
	if runErr != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			return nil, fmt.Errorf("ai: vmafx-ort-runner cancelled: %w (run err: %v)", ctxErr, runErr)
		}
		// The runner explains itself on stderr ("libvmaf was built without
		// DNN support", "model not found", ...); cmd.Output captured it, so
		// surface it instead of a bare "exit status N".
		var exitErr *exec.ExitError
		if errors.As(runErr, &exitErr) {
			if msg := strings.TrimSpace(string(exitErr.Stderr)); msg != "" {
				return nil, fmt.Errorf("ai: vmafx-ort-runner failed: %w: %s", runErr, msg)
			}
		}
		return nil, fmt.Errorf("ai: vmafx-ort-runner failed: %w", runErr)
	}

	var outputs []float64
	if err := json.Unmarshal(out, &outputs); err != nil {
		return nil, fmt.Errorf("ai: parse ort-runner output: %w; raw=%s", err, string(out))
	}
	return outputs, nil
}

// ErrORTRunnerNotFound is returned by Infer when vmafx-ort-runner is absent
// from PATH.  Callers can test for this error to decide whether to skip AI
// inference or return an unsupported error to the controller.
var ErrORTRunnerNotFound = fmt.Errorf("ai: vmafx-ort-runner not found on PATH")

// InferDirect is the Stage 2 direct CGO path via github.com/yalue/onnxruntime_go.
// It is not yet implemented and always returns ErrDirectInferNotImplemented.
// This stub exists to anchor the Stage 2 refactor.
func (r *Registry) InferDirect(_ string, _ []float64) ([]float64, error) {
	return nil, ErrDirectInferNotImplemented
}

// ErrDirectInferNotImplemented is returned by InferDirect until Stage 2.
var ErrDirectInferNotImplemented = fmt.Errorf("ai: direct ORT CGO path not yet implemented (Stage 2)")

// ListModels returns the names of .onnx models in the registry's model directory.
// Returns an empty slice (not an error) when the directory is absent or empty.
//
// Deprecated: prefer ListModelsSeq for new code.  ListModels eagerly walks
// the directory and materialises a slice even when the caller breaks out
// of the range early (e.g. "is model X registered?").  ListModelsSeq is
// the streaming-friendly equivalent that defers each os.ReadDir entry
// inspection to the yield call.  ListModels remains supported for one
// release as a shim and will be removed no earlier than v4.0.0 (ADR-0932).
func (r *Registry) ListModels() []string {
	return slices.Collect(r.ListModelsSeq())
}

// ListModelsSeq returns a single-shot iterator over the names of .onnx
// models in the registry's model directory, in os.ReadDir order.
// Yields nothing when the directory is absent or unreadable.
//
// Added in v3.2.1 (ADR-0932). Callers iterating linearly
// (filter, dispatch, early-break) should prefer this over ListModels.
func (r *Registry) ListModelsSeq() iter.Seq[string] {
	return func(yield func(string) bool) {
		entries, err := os.ReadDir(r.modelDir)
		if err != nil {
			return
		}
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".onnx") {
				continue
			}
			if !yield(strings.TrimSuffix(e.Name(), ".onnx")) {
				return
			}
		}
	}
}

// InputCount reads the expected number of inputs for a model from the sidecar
// JSON file at <modelDir>/<modelName>.json.  Returns 0 and no error when no
// sidecar exists (callers can proceed without input validation).
func (r *Registry) InputCount(modelName string) (int, error) {
	sidecar := filepath.Join(r.modelDir, modelName+".json")
	// #nosec G304 -- sidecar is filepath.Join(r.modelDir, modelName+".json")
	// where r.modelDir is registry-controlled at construction. Caller must
	// have first resolved modelName via Registry.ModelPath which enforces
	// the same prefix.
	data, err := os.ReadFile(sidecar)
	if os.IsNotExist(err) {
		return 0, nil
	}
	if err != nil {
		return 0, fmt.Errorf("ai: read sidecar %s: %w", sidecar, err)
	}
	var meta struct {
		InputCount int `json:"input_count"`
	}
	if err := json.Unmarshal(data, &meta); err != nil {
		return 0, fmt.Errorf("ai: parse sidecar %s: %w", sidecar, err)
	}
	return meta.InputCount, nil
}

// floatsToCSV formats a float64 slice as a comma-separated string — used in
// diagnostic logging.
func floatsToCSV(vs []float64) string {
	ss := make([]string, len(vs))
	for i, v := range vs {
		ss[i] = strconv.FormatFloat(v, 'g', -1, 64)
	}
	return strings.Join(ss, ",")
}

// DiagString returns a human-readable summary of an inference call for logs.
func DiagString(modelName string, inputs, outputs []float64) string {
	return fmt.Sprintf("model=%s inputs=[%s] outputs=[%s]",
		modelName, floatsToCSV(inputs), floatsToCSV(outputs))
}
