// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package predictor

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	"github.com/VMAFx/vmafx/pkg/ai"
)

// ORTSession adapts pkg/ai's ONNX Runtime bridge to the Session seam.
//
// The Python Predictor loads the model with onnxruntime in-process. Go has no
// in-process ORT without a cgo binding, so the fork routes inference through
// the vmafx-ort-runner subprocess (ADR-0713); pkg/ai owns that bridge and its
// timeout. When the runner is absent from PATH, Infer reports
// ai.ErrORTRunnerNotFound and PredictVMAF degrades to the analytical curve —
// what the Python does when onnxruntime is not installed — while recording the
// failure once through SessionFailed and the optional Log.
//
// This is the one ORT adapter in the tree (ADR-1137): the `predict`, `auto`
// and `sidecar` subcommands all attach it through NewORTSession or
// NewWithModel. It used to live in cmd/vmafx-tune/cmd next to a second,
// registry-based ONNX path inside the deleted pkg/tune/predictor.
type ORTSession struct {
	ctx       context.Context
	registry  *ai.Registry
	modelPath string
}

// NewORTSession returns a Session for the model at modelPath, or nil when no
// model was requested, so `WithSession(NewORTSession(ctx, ""))` is the
// analytical predictor.
//
// It does NOT probe for the runner here: the Python constructor is equally
// lazy, and probing would make `--model` fail on hosts where the runner
// appears later in the run. The first Infer call surfaces the problem, and
// PredictVMAF falls back to the analytical curve on any inference error.
func NewORTSession(ctx context.Context, modelPath string) Session {
	if modelPath == "" {
		return nil
	}
	return &ORTSession{
		ctx:       ctx,
		registry:  ai.NewRegistry(""),
		modelPath: modelPath,
	}
}

// NewWithModel builds the predictor a `--model`-bearing subcommand runs on:
// the analytical curve when modelPath is empty, otherwise the curve with an
// ORTSession attached and log receiving the one-time fallback warning.
//
// The model path is resolved through the registry up front so a `--model`
// that names nothing fails the way the Python constructor's
// FileNotFoundError does, while the runner itself stays lazy (see
// NewORTSession).
func NewWithModel(ctx context.Context, modelPath string, log *slog.Logger) (*Predictor, error) {
	if modelPath == "" {
		return New(), nil
	}
	if _, err := ai.NewRegistry("").ModelPath(modelPath); err != nil {
		return nil, err
	}
	pred := WithSession(NewORTSession(ctx, modelPath))
	pred.Log = log
	return pred, nil
}

// Infer runs one forward pass and returns the model's scalar output.
func (s *ORTSession) Infer(inputs []float64) (float64, error) {
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
