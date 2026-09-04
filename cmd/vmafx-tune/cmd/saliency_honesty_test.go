// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// Pins that the saliency surfaces report what actually happened and that
// predict wires saliency moments correctly into feature extraction.

package cmd

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/predictor"
	"github.com/VMAFx/vmafx/pkg/saliency"
)

func TestPredict_rejectsNonexistentSaliencyModel(t *testing.T) {
	t.Parallel()

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"predict", "--use-saliency", "--saliency-model", "/nonexistent/model.onnx",
		"--source", "nonexistent.mp4", "--codec", "libx264",
	})
	root.Cobra().SetOut(&strings.Builder{})
	root.Cobra().SetErr(&strings.Builder{})

	err := root.Execute()
	if err == nil {
		t.Fatal("predict --saliency-model with nonexistent file returned nil; want a usage failure")
	}
	if !strings.Contains(err.Error(), "/nonexistent/model.onnx") {
		t.Errorf("diagnostic should name the missing file, got: %v", err)
	}
	var coded exitCodeError
	if !errors.As(err, &coded) {
		var codedPtr *exitCodeError
		if !errors.As(err, &codedPtr) {
			t.Fatalf("error should carry an exit code, got %T", err)
		}
		coded = *codedPtr
	}
	if got := coded.ExitCode(); got != usageExitCode {
		t.Errorf("exit code = %d, want %d (usage)", got, usageExitCode)
	}
}

func TestComputeSaliencyMoments(t *testing.T) {
	t.Parallel()

	mean, variance := computeSaliencyMoments(nil)
	if mean != 0.0 || variance != 0.0 {
		t.Errorf("empty slice = (%v, %v), want (0, 0)", mean, variance)
	}

	values := []float64{1.0, 2.0, 3.0, 4.0, 5.0}
	mean, variance = computeSaliencyMoments(values)
	if math.Abs(mean-3.0) > 1e-9 {
		t.Errorf("mean = %v, want 3.0", mean)
	}
	if math.Abs(variance-2.0) > 1e-9 {
		t.Errorf("variance = %v, want 2.0", variance)
	}
}

type stubSaliencySession struct {
	value float32
}

func (s *stubSaliencySession) Run(input []float32, height, width int) ([]float32, error) {
	out := make([]float32, height*width)
	for i := range out {
		out[i] = s.value
	}
	return out, nil
}

func TestNewPredictSaliencyFunc_WithSession(t *testing.T) {
	origFactory := saliencySessionFactory
	t.Cleanup(func() { saliencySessionFactory = origFactory })

	saliencySessionFactory = func(modelPath string) (saliency.Session, error) {
		return &stubSaliencySession{value: 0.6}, nil
	}

	dir := t.TempDir()
	yuvPath := filepath.Join(dir, "test.yuv")
	frameBytes := saliency.FrameSizeBytes(64, 64) * 4
	if err := os.WriteFile(yuvPath, make([]byte, frameBytes), 0o600); err != nil {
		t.Fatalf("write yuv: %v", err)
	}

	d := deps{Log: slog.New(slog.NewTextHandler(io.Discard, nil))}
	salFunc := newPredictSaliencyFunc(context.Background(), d)

	mean, variance, err := salFunc(yuvPath, 64, 64, 2, "mock.onnx")
	if err != nil {
		t.Fatalf("salFunc: %v", err)
	}
	if math.Abs(mean-0.6) > 1e-4 {
		t.Errorf("mean = %v, want ~0.6", mean)
	}
	if math.Abs(variance) > 1e-4 {
		t.Errorf("variance = %v, want ~0.0", variance)
	}
}

func TestNewPredictSaliencyFunc_UnavailableDegradesGracefully(t *testing.T) {
	origFactory := saliencySessionFactory
	t.Cleanup(func() { saliencySessionFactory = origFactory })

	saliencySessionFactory = func(modelPath string) (saliency.Session, error) {
		return nil, ErrSaliencyInferenceUnavailable
	}

	dir := t.TempDir()
	yuvPath := filepath.Join(dir, "test.yuv")
	frameBytes := saliency.FrameSizeBytes(64, 64) * 2
	if err := os.WriteFile(yuvPath, make([]byte, frameBytes), 0o600); err != nil {
		t.Fatalf("write yuv: %v", err)
	}

	d := deps{Log: slog.New(slog.NewTextHandler(io.Discard, nil))}
	salFunc := newPredictSaliencyFunc(context.Background(), d)

	mean, variance, err := salFunc(yuvPath, 64, 64, 2, "mock.onnx")
	if err != nil {
		t.Fatalf("salFunc should degrade gracefully with nil error, got: %v", err)
	}
	if mean != 0.0 || variance != 0.0 {
		t.Errorf("degraded moments = (%v, %v), want (0, 0)", mean, variance)
	}
}

func TestPredict_ExtractFeatures_WiresSaliencyMoments(t *testing.T) {
	origFactory := saliencySessionFactory
	t.Cleanup(func() { saliencySessionFactory = origFactory })

	saliencySessionFactory = func(modelPath string) (saliency.Session, error) {
		return &stubSaliencySession{value: 0.75}, nil
	}

	dir := t.TempDir()
	mockRunner := func(ctx context.Context, argv []string) (stdout, stderr string, exitStatus int, err error) {
		for i, arg := range argv {
			if arg == "-f" && i+2 < len(argv) && argv[i+1] == "rawvideo" {
				outPath := argv[i+2]
				frameBytes := saliency.FrameSizeBytes(64, 64) * 4
				if writeErr := os.WriteFile(outPath, make([]byte, frameBytes), 0o600); writeErr != nil {
					return "", "", 1, writeErr
				}
				return "", "", 0, nil
			}
		}
		return "", "", 0, nil
	}

	d := deps{Log: slog.New(slog.NewTextHandler(io.Discard, nil))}
	salFunc := newPredictSaliencyFunc(context.Background(), d)

	shot := pershot.Shot{StartFrame: 0, EndFrame: 10}
	geometry := predictor.Geometry{Width: 64, Height: 64, FPS: 24.0}
	cfg := predictor.ExtractorConfig{
		UseSaliency:          true,
		SaliencyFrameSamples: 2,
		ProbeMaxFrames:       240,
	}

	features, err := predictor.ExtractFeatures(
		context.Background(), shot, filepath.Join(dir, "dummy.mp4"), "libx264",
		geometry, cfg, mockRunner, salFunc,
	)
	if err != nil {
		t.Fatalf("ExtractFeatures: %v", err)
	}
	if math.Abs(features.SaliencyMean-0.75) > 1e-4 {
		t.Errorf("SaliencyMean = %v, want ~0.75", features.SaliencyMean)
	}
	if math.Abs(features.SaliencyVar) > 1e-4 {
		t.Errorf("SaliencyVar = %v, want ~0.0", features.SaliencyVar)
	}

	vec := predictor.FeatureVector(features, 23)
	if math.Abs(vec[5]-0.75) > 1e-4 {
		t.Errorf("FeatureVector[5] (saliency_mean) = %v, want ~0.75", vec[5])
	}
	if math.Abs(vec[6]-0.0) > 1e-4 {
		t.Errorf("FeatureVector[6] (saliency_var) = %v, want ~0.0", vec[6])
	}
}
