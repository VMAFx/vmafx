// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package predictor

import (
	"bytes"
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
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

func TestIsStubPredictorModel(t *testing.T) {
	t.Parallel()
	// Empty path
	if IsStubPredictorModel("") {
		t.Errorf("IsStubPredictorModel(\"\") = true, want false")
	}
	// Software codecs are stubs
	if !IsStubPredictorModel("model/predictor_libx264.onnx") {
		t.Errorf("IsStubPredictorModel(predictor_libx264) = false, want true")
	}
	if !IsStubPredictorModel("model/predictor_h264_amf.onnx") {
		t.Errorf("IsStubPredictorModel(predictor_h264_amf) = false, want true")
	}
	// Hardware nvenc/qsv are real models
	if IsStubPredictorModel("model/predictor_h264_nvenc.onnx") {
		t.Errorf("IsStubPredictorModel(predictor_h264_nvenc) = true, want false")
	}
	if IsStubPredictorModel("model/predictor_av1_qsv.onnx") {
		t.Errorf("IsStubPredictorModel(predictor_av1_qsv) = true, want false")
	}
}

func TestIsStubPredictorModelCard(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct {
		name, model, card string
		want              bool
	}{
		{"synthetic custom model", "custom.onnx", "corpus.kind: synthetic-stub-N=100", true},
		{"real overrides codec fallback", "predictor_libx264.onnx", "corpus.kind: real-N=200", false},
		{"synthetic takes precedence", "custom.onnx", "real-N=200 synthetic-stub-N=100", true},
		{"unrecognized card keeps codec fallback", "predictor_libx264.onnx", "unknown", true},
		{"filename stub takes precedence", "custom_stub.onnx", "real-N=200", true},
		{"multi-dot basename", "custom.v2.onnx", "synthetic-stub-N=100", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			dir := t.TempDir()
			modelPath := filepath.Join(dir, tc.model)
			cardPath := modelPath[:len(modelPath)-len(filepath.Ext(modelPath))] + "_card.md"
			if err := os.WriteFile(cardPath, []byte(tc.card), 0o600); err != nil {
				t.Fatal(err)
			}
			if got := IsStubPredictorModel(modelPath); got != tc.want {
				t.Errorf("IsStubPredictorModel(%q) = %v, want %v", modelPath, got, tc.want)
			}
		})
	}
}

func TestIsStubPredictorModelCardSymlinks(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct {
		name, target string
		want         bool
	}{
		{"inside model directory", "card.md", true},
		{"outside model directory", "../card.md", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			dir := t.TempDir()
			modelDir := filepath.Join(dir, "models")
			if err := os.Mkdir(modelDir, 0o700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(modelDir, tc.target), []byte("synthetic-stub-N=100"), 0o600); err != nil {
				t.Fatal(err)
			}
			if err := os.Symlink(tc.target, filepath.Join(modelDir, "custom_card.md")); err != nil {
				t.Skipf("symlink unavailable: %v", err)
			}
			if got := IsStubPredictorModel(filepath.Join(modelDir, "custom.onnx")); got != tc.want {
				t.Errorf("IsStubPredictorModel with card symlink %q = %v, want %v", tc.target, got, tc.want)
			}
		})
	}
}

func TestIsStubPredictorModelMissingCardFallback(t *testing.T) {
	t.Parallel()
	for _, dir := range []string{t.TempDir(), filepath.Join(t.TempDir(), "missing")} {
		if !IsStubPredictorModel(filepath.Join(dir, "predictor_libx264.onnx")) {
			t.Error("missing card must preserve the known-codec stub fallback")
		}
		if IsStubPredictorModel(filepath.Join(dir, "custom.onnx")) {
			t.Error("missing card must not classify a custom model as a known stub")
		}
	}
}

func TestNewWithModelUsesResolvedModelCard(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("VMAFX_MODEL_DIR", dir)
	if err := os.WriteFile(filepath.Join(dir, "custom.onnx"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "custom_card.md"), []byte("synthetic-stub-N=100"), 0o600); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	log := slog.New(slog.NewTextHandler(&output, nil))
	if _, err := NewWithModel(context.Background(), "custom", log); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(output.String(), "loading synthetic-stub model") {
		t.Errorf("registry-resolved model did not emit its card's stub warning: %s", output.String())
	}
}
