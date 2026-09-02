// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package predictor

import (
	"context"
	"path/filepath"
	"testing"
)

// TestNewORTSessionEmptyPathIsNil pins the `--model ""` contract: no model
// requested means no session, so WithSession(NewORTSession(ctx, "")) is the
// analytical predictor rather than one that fails on every inference.
func TestNewORTSessionEmptyPathIsNil(t *testing.T) {
	t.Parallel()
	if s := NewORTSession(context.Background(), ""); s != nil {
		t.Fatalf("NewORTSession(\"\") = %#v, want nil", s)
	}
}

// TestNewWithModelEmptyPathIsAnalytical pins that NewWithModel with no model
// path is exactly New(): shipped coefficients, no session, no error — the
// Python Predictor() with model_path=None.
func TestNewWithModelEmptyPathIsAnalytical(t *testing.T) {
	t.Parallel()
	p, err := NewWithModel(context.Background(), "", nil)
	if err != nil {
		t.Fatalf("NewWithModel(\"\"): %v", err)
	}
	if p.Session != nil {
		t.Errorf("Session = %#v, want nil for the analytical fallback", p.Session)
	}
	want := New()
	feat := ShotFeatures{ProbeBitrateKbps: 905.82}
	if got, exp := p.PredictVMAF(feat, 23, "libx264"), want.PredictVMAF(feat, 23, "libx264"); got != exp {
		t.Errorf("PredictVMAF = %v, want New()'s %v", got, exp)
	}
}

// TestNewWithModelMissingModelIsAnError pins the Python constructor's
// FileNotFoundError: a --model that resolves to nothing fails the command
// instead of silently running the analytical curve under a model's name.
// (Carried over from the deleted pkg/tune/predictor's TestNewWithMissingModel.)
func TestNewWithModelMissingModelIsAnError(t *testing.T) {
	t.Parallel()
	missing := filepath.Join(t.TempDir(), "predictor_libx264.onnx")
	p, err := NewWithModel(context.Background(), missing, nil)
	if err == nil {
		t.Fatalf("NewWithModel(%q) = %#v, nil; want an error for a missing model", missing, p)
	}
}
