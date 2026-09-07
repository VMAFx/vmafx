// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package predictor

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"

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
// and `sidecar` subcommands all route through it rather than reimplementing
// the bridge.

type ORTSession struct {
	ctx       context.Context
	registry  *ai.Registry
	modelPath string
}

// NewORTSession constructs the ORT bridge for modelPath. The subprocess is
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

// IsStubPredictorModel reports whether modelPath points to a synthetic-stub model.
//
// Software models (libx264, libx265, libsvtav1, libaom-av1, libvvenc) and AMF models
// (h264_amf, hevc_amf, av1_amf) ship synthetic stubs trained on the analytical curve
// (ADR-0325). They are not authoritative for production CRF picks.
func IsStubPredictorModel(modelPath string) bool {
	if modelPath == "" {
		return false
	}
	base := filepath.Base(modelPath)
	if strings.Contains(strings.ToLower(base), "stub") {
		return true
	}
	ext := filepath.Ext(modelPath)
	stem := strings.TrimSuffix(modelPath, ext)
	cardPath := stem + "_card.md"
	if data, err := os.ReadFile(cardPath); err == nil {
		cardText := string(data)
		if strings.Contains(cardText, "synthetic-stub") {
			return true
		}
		if strings.Contains(cardText, "real-N=") {
			return false
		}
	}
	knownStubCodecs := []string{
		"libx264", "libx265", "libsvtav1", "libaom-av1", "libvvenc",
		"h264_amf", "hevc_amf", "av1_amf",
	}
	for _, codec := range knownStubCodecs {
		if strings.Contains(base, codec) {
			return true
		}
	}
	return false
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
	if IsStubPredictorModel(modelPath) {
		if log != nil {
			log.Warn("predictor: loading synthetic-stub model; not authoritative for production CRF picks",
				"model", modelPath)
		}
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
