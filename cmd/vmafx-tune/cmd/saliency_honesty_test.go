// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// Pins that the saliency surfaces report what actually happened.

package cmd

import (
	"errors"
	"strings"
	"testing"
)

// TestPredict_rejectsUseSaliency pins that --use-saliency fails rather than
// being accepted and ignored. predictor.ExtractorConfig gates the saliency pass
// on `cfg.UseSaliency && saliency != nil`, and no production caller supplies a
// predictor.SaliencyFunc -- it is referenced only by features.go and its own
// test -- so the flag used to leave the saliency mean/variance features at 0.0
// for every shot while the run reported success.
func TestPredict_rejectsUseSaliency(t *testing.T) {
	t.Parallel()

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"predict", "--use-saliency",
		"--source", "nonexistent.mp4", "--codec", "libx264",
	})
	root.Cobra().SetOut(&strings.Builder{})
	root.Cobra().SetErr(&strings.Builder{})

	err := root.Execute()
	if err == nil {
		t.Fatal("predict --use-saliency returned nil; want a usage failure")
	}
	if !strings.Contains(err.Error(), "--use-saliency is not implemented") {
		t.Errorf("diagnostic should name the unimplemented flag, got: %v", err)
	}
	if !strings.Contains(err.Error(), "vmaf-tune predict --use-saliency") {
		t.Errorf("diagnostic should name the Python fallback, got: %v", err)
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
