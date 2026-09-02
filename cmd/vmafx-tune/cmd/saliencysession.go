// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/VMAFx/vmafx/pkg/saliency"
)

// ErrSaliencyInferenceUnavailable explains why the Go port cannot yet run the
// saliency_student_v1 model in-process, and what would unblock it.
//
// The whole numeric pipeline around the model IS ported and tested against the
// Python (pkg/saliency): YUV to ImageNet tensor, the four temporal
// aggregators, the QP mapping, the per-block reduce, and all five encoder
// sidecar formats. Only the single forward pass is missing, and it is missing
// for a concrete, non-hand-wavy reason:
//
//   - Go has no in-process ONNX Runtime in this module. The fork's existing
//     bridge (pkg/ai, ADR-0713) shells out to vmafx-ort-runner and passes the
//     input tensor as a JSON array in argv. That is fine for the per-shot
//     VMAF predictor's 14 floats; it cannot carry saliency's 3xHxW input,
//     which is 6.2 million floats (about 75 MB of JSON) for a 1080p frame,
//     far past any platform's ARG_MAX.
//   - vmafx-ort-runner is also not built from this repository: nothing under
//     cmd/ produces it, so even the small-tensor path depends on a binary
//     supplied by the container image.
//
// Two things would unblock it, either alone:
//
//  1. A cgo ONNX Runtime binding (github.com/yalue/onnxruntime_go resolves
//     from the module proxy and dlopens libonnxruntime at runtime), which
//     makes cmd/vmafx-tune a cgo build — an ADR-level decision, since the
//     binary currently builds without cgo.
//  2. A vmafx-ort-runner protocol that streams tensors over stdin or a file
//     handle instead of argv, plus a cmd/vmafx-ort-runner target in this
//     repository.
//
// Until then the CLI degrades to a plain encode with a warning, which is
// exactly what the Python does when onnxruntime is not installed.
var ErrSaliencyInferenceUnavailable = errors.New(
	"no in-process ONNX Runtime is available to the Go build, and the " +
		"vmafx-ort-runner subprocess passes tensors through argv, which " +
		"cannot carry a full-resolution saliency input")

// resolveSaliencyModel locates the model file, walking up from the working
// directory to find the repo-relative default when no explicit path is given.
func resolveSaliencyModel(modelPath string) (string, error) {
	if modelPath != "" {
		if _, err := os.Stat(modelPath); err != nil {
			return "", fmt.Errorf("saliency model %q: %w", modelPath, err)
		}
		return modelPath, nil
	}
	dir, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("resolve saliency model: %w", err)
	}
	for {
		candidate := filepath.Join(dir, saliency.DefaultModelRelPath)
		if info, statErr := os.Stat(candidate); statErr == nil && !info.IsDir() {
			return candidate, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("saliency model not found: no %s above %s",
				saliency.DefaultModelRelPath, dir)
		}
		dir = parent
	}
}

// newSaliencySession returns an inference session for the saliency model.
//
// It always reports ErrSaliencyInferenceUnavailable today — see that
// variable's comment for exactly what is missing and what would unblock it.
// The model path is still resolved first so an operator who passes a wrong
// --saliency-model gets that error rather than the generic one.
func newSaliencySession(modelPath string) (saliency.Session, error) {
	resolved, err := resolveSaliencyModel(modelPath)
	if err != nil {
		return nil, err
	}
	return nil, fmt.Errorf("saliency model %s cannot be run: %w",
		resolved, ErrSaliencyInferenceUnavailable)
}
