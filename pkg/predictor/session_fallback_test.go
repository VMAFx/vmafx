// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package predictor

import (
	"errors"
	"testing"
)

type failingSession struct{ calls int }

func (f *failingSession) Infer([]float64) (float64, error) {
	f.calls++
	return 0, errors.New("vmafx-ort-runner is not on PATH")
}

// TestSessionFailureIsReported pins that a configured-but-unusable ONNX session
// does not degrade silently. PredictVMAF discarded the inference error, so
// `predict --model` announced the learned predictor and then returned
// analytical values for the whole sweep with nothing to distinguish it from a
// real model-backed run.
func TestSessionFailureIsReported(t *testing.T) {
	t.Parallel()

	sess := &failingSession{}
	p := WithSession(sess)

	feat := ShotFeatures{}
	first := p.PredictVMAF(feat, 23, "libx264")
	second := p.PredictVMAF(feat, 28, "libx264")

	if p.SessionFailed() == nil {
		t.Error("SessionFailed() = nil after every inference failed; the fallback is still silent")
	}
	if sess.calls < 2 {
		t.Errorf("session consulted %d times, want 2 (it must keep trying, not latch off)", sess.calls)
	}
	// Falling back is correct — Python does too. Reporting it is the fix.
	if first <= 0 || second <= 0 {
		t.Errorf("analytical fallback produced %v / %v, want positive VMAF estimates", first, second)
	}
}
