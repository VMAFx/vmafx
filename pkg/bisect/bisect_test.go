// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package bisect_test

import (
	"fmt"
	"os"
	"testing"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/encoder"
)

// mockEncoder is a test double for encoder.Encoder that records the CRFs it
// was called with and returns deterministic results.
type mockEncoder struct {
	name     string
	callCRFs []int
	// vmafByCRF allows the test to control the simulated VMAF response.
	// VMAF is modelled as monotone-decreasing in CRF.
	vmafByCRF func(crf int) float64
}

func (m *mockEncoder) Name() string        { return m.name }
func (m *mockEncoder) CRFRange() (int, int) { return 0, 51 }

func (m *mockEncoder) Encode(src string, params encoder.EncodeParams) (encoder.EncodeResult, error) {
	m.callCRFs = append(m.callCRFs, params.CRF)
	// Write a tiny placeholder file so the score function can reference it.
	tmp, err := os.CreateTemp("", fmt.Sprintf("mock-enc-crf%d-*.mkv", params.CRF))
	if err != nil {
		return encoder.EncodeResult{}, fmt.Errorf("mock encoder tempfile: %w", err)
	}
	_ = tmp.Close()
	// Simulate bitrate inversely proportional to CRF.
	bitrate := 10000.0 / float64(params.CRF+1)
	return encoder.EncodeResult{
		OutputPath:     tmp.Name(),
		BitratekBps:    bitrate,
		EncoderVersion: "mock v1",
		EncodeTimeMS:   1.0,
	}, nil
}

// mockScoreFunc returns a ScoreFunc that uses vmafByCRF to return a score
// based on the CRF embedded in the distorted file's name.
func mockScoreFunc(vmafByCRF func(crf int) float64) bisect.ScoreFunc {
	return func(ref, distorted string) (float64, error) {
		// The mock encoder encodes CRF into the filename; extract it.
		var crf int
		_, err := fmt.Sscanf(distorted, os.TempDir()+"/mock-enc-crf%d-", &crf)
		if err != nil {
			// Fallback: parse from the end of the file name.
			// Since os.TempDir() may have subdirs, just return a fixed score.
			return 85.0, nil
		}
		return vmafByCRF(crf), nil
	}
}

// linearVMAF returns a VMAF function that decreases linearly from 100 at
// CRF=0 to 0 at CRF=100.
func linearVMAF(crf int) float64 {
	v := 100.0 - float64(crf)*2.0
	if v < 0 {
		return 0
	}
	return v
}

// TestBisect_convergence verifies that Bisect converges on the expected CRF
// for a simple linear VMAF model and target of 85.
//
// VMAF = 100 - 2*CRF => CRF that gives VMAF=85 is CRF=7 (VMAF=86).
// The bisect should find CRF=7 as the highest CRF where VMAF >= 85.
func TestBisect_convergence(t *testing.T) {
	t.Parallel()

	enc := &mockEncoder{name: "libx264", vmafByCRF: linearVMAF}

	result, err := bisect.Run(
		"/dev/null",
		enc,
		func(ref, distorted string) (float64, error) {
			// Extract CRF from the CRF list recorded by the encoder.
			// Since score is called right after encode, the last recorded CRF
			// matches this distorted file.
			if len(enc.callCRFs) == 0 {
				return 0, fmt.Errorf("no CRFs recorded yet")
			}
			crf := enc.callCRFs[len(enc.callCRFs)-1]
			return linearVMAF(crf), nil
		},
		bisect.Params{
			TargetVMAF: 85.0,
			CRFLo:      0,
			CRFHi:      51,
			MaxIter:    bisect.DefaultMaxIter,
		},
	)
	if err != nil {
		t.Fatalf("bisect.Run: %v", err)
	}

	if result.BestCRF < 0 {
		t.Fatal("expected a satisfying CRF, got BestCRF=-1")
	}
	if result.BestVMAFScore < 85.0 {
		t.Errorf("BestVMAFScore = %f, want >= 85.0", result.BestVMAFScore)
	}
	// BestCRF should be the highest CRF where VMAF >= 85.
	// For linearVMAF: VMAF(7) = 86, VMAF(8) = 84 → BestCRF = 7.
	if result.BestCRF != 7 {
		t.Errorf("BestCRF = %d, want 7", result.BestCRF)
	}
	if result.Iterations <= 0 {
		t.Error("Iterations must be > 0")
	}
	if len(result.Samples) == 0 {
		t.Error("Samples must not be empty")
	}
	t.Logf("converged in %d iterations; BestCRF=%d, BestVMAF=%.2f",
		result.Iterations, result.BestCRF, result.BestVMAFScore)
}

// TestBisect_noSatisfyingCRF verifies that Bisect returns BestCRF=-1 when no
// CRF within the search window meets the target.
func TestBisect_noSatisfyingCRF(t *testing.T) {
	t.Parallel()

	enc := &mockEncoder{name: "libx264", vmafByCRF: linearVMAF}

	result, err := bisect.Run(
		"/dev/null",
		enc,
		func(ref, distorted string) (float64, error) {
			if len(enc.callCRFs) == 0 {
				return 0, fmt.Errorf("no CRFs recorded")
			}
			crf := enc.callCRFs[len(enc.callCRFs)-1]
			return linearVMAF(crf), nil
		},
		bisect.Params{
			// Target 99: only reachable at CRF=0 (VMAF=100) — but let's
			// narrow the window to CRF[20,51] so no CRF meets the target.
			TargetVMAF: 99.0,
			CRFLo:      20,
			CRFHi:      51,
			MaxIter:    bisect.DefaultMaxIter,
		},
	)
	if err != nil {
		t.Fatalf("bisect.Run: %v", err)
	}

	if result.BestCRF != -1 {
		t.Errorf("expected BestCRF=-1 (no satisfying CRF), got %d", result.BestCRF)
	}
}

// TestBisect_invalidParams checks that Bisect rejects nonsensical parameters
// without panicking.
func TestBisect_invalidParams(t *testing.T) {
	t.Parallel()

	enc := &mockEncoder{name: "libx264", vmafByCRF: linearVMAF}
	noopScore := func(string, string) (float64, error) { return 90.0, nil }

	t.Run("target out of range", func(t *testing.T) {
		t.Parallel()
		_, err := bisect.Run("/dev/null", enc, noopScore, bisect.Params{
			TargetVMAF: 0.0, // invalid
			CRFLo: 0, CRFHi: 51,
		})
		if err == nil {
			t.Error("expected error for TargetVMAF=0, got nil")
		}
	})

	t.Run("CRFLo > CRFHi", func(t *testing.T) {
		t.Parallel()
		_, err := bisect.Run("/dev/null", enc, noopScore, bisect.Params{
			TargetVMAF: 85.0,
			CRFLo: 51, CRFHi: 0,
		})
		if err == nil {
			t.Error("expected error for CRFLo > CRFHi, got nil")
		}
	})
}
