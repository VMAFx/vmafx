// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"

	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/predictor"
)

// ortPredictorSession adapts pkg/ai's ONNX Runtime bridge to the
// predictor.Session seam.
//
// The Python Predictor loads the model with onnxruntime in-process. Go has no
// in-process ORT without a cgo binding, so the fork routes inference through
// the vmafx-ort-runner subprocess (ADR-0713); pkg/ai owns that bridge and its
// timeout. When the runner is absent from PATH, Infer reports
// ai.ErrORTRunnerNotFound and the caller degrades to the analytical curve —
// exactly what the Python does when onnxruntime is not installed.
type ortPredictorSession struct {
	ctx       context.Context
	registry  *ai.Registry
	modelPath string
}

// newORTPredictorSession returns a session for the model at modelPath, or nil
// when no model was requested.
//
// It does NOT probe for the runner here: the Python constructor is equally
// lazy, and probing would make `--model` fail on hosts where the runner
// appears later in the run. The first Infer call surfaces the problem, and
// PredictVMAF falls back to the analytical curve on any inference error.
func newORTPredictorSession(ctx context.Context, modelPath string) predictor.Session {
	if modelPath == "" {
		return nil
	}
	return &ortPredictorSession{
		ctx:       ctx,
		registry:  ai.NewRegistry(""),
		modelPath: modelPath,
	}
}

// Infer runs one forward pass and returns the model's scalar output.
func (s *ortPredictorSession) Infer(inputs []float64) (float64, error) {
	outputs, err := s.registry.Infer(s.ctx, s.modelPath, inputs)
	if err != nil {
		if errors.Is(err, ai.ErrORTRunnerNotFound) {
			return 0, fmt.Errorf(
				"predictor model %q requested but vmafx-ort-runner is not on "+
					"PATH; falling back to the analytical curve: %w",
				s.modelPath, err)
		}
		return 0, err
	}
	if len(outputs) == 0 {
		return 0, fmt.Errorf("predictor model %q returned no outputs", s.modelPath)
	}
	return outputs[0], nil
}
