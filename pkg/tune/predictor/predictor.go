// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package predictor is a transitional alias of pkg/predictor.
//
// The VMAF predictor — ShotFeatures, the per-codec analytical curve with its
// pymath.Log10 parity term, the ONNX session seam and the PickCRF inversion —
// has one implementation, pkg/predictor (ADR-1137); the parallel port's
// second copy that lived here was deleted, and its Python-derived fixture
// moved to pkg/predictor/testdata. This package remains only because
// pkg/tune/sidecar and cmd/vmafx-tune/cmd/sidecar.go still import it and
// those files are owned by an in-flight PR (#1187, the sidecar Python-parity
// fix); repointing them here would conflict with it. Once #1187 lands, the
// sidecar imports move to pkg/predictor and this package is deleted. Do not
// add anything to it.
package predictor

import (
	"context"
	"log/slog"

	"github.com/VMAFx/vmafx/pkg/predictor"
)

// ShotFeatures is predictor.ShotFeatures.
type ShotFeatures = predictor.ShotFeatures

// Predictor is predictor.Predictor. Its zero value is the analytical
// fallback, which is what the sidecar tests construct.
type Predictor = predictor.Predictor

// Coefficients is predictor.Coefficients.
type Coefficients = predictor.Coefficients

// New is the constructor the sidecar CLI calls: predictor.NewWithModel with a
// background context. modelPath "" is the analytical curve; a model path that
// does not resolve is an error, matching the Python FileNotFoundError; a
// resolvable one attaches the ORT session lazily. A nil log falls back to
// slog.Default so a degraded --model run is still reported once.
func New(modelPath string, log *slog.Logger) (*Predictor, error) {
	if log == nil {
		log = slog.Default()
	}
	return predictor.NewWithModel(context.Background(), modelPath, log)
}

// PickCRF is (*predictor.Predictor).PickCRF under the old package-level name.
func PickCRF(p *Predictor, features ShotFeatures, targetVMAF float64, codecName string) (int, error) {
	return p.PickCRF(features, targetVMAF, codecName)
}

// ResolutionClass is predictor.ResolutionClass.
func ResolutionClass(height int) string {
	return predictor.ResolutionClass(height)
}
