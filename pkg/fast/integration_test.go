// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/fast/integration_test.go — end-to-end exercise of the production
// probe-encode / decode / libvmaf-score pipeline against the real ffmpeg and
// vmaf binaries.
//
// These tests skip when the tools or the raw-YUV fixture are unavailable, so
// they are safe on a bare CI runner but give real coverage in the dev
// container (CLAUDE.md §12 r15), where both binaries are present. Only the
// ONNX proxy is stubbed — that is the one step the Go tree cannot drive yet
// (see ErrProxyPortsUnsupported).

package fast

import (
	"context"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// fixtureRef is a 48-frame 576x324 8-bit 4:2:0 raw YUV shipped in-tree.
const (
	fixtureRel    = "../../testdata/ref_576x324_48f.yuv"
	fixtureWidth  = 576
	fixtureHeight = 324
	fixtureFPS    = 24.0
)

// requireTools skips the test unless ffmpeg, vmaf and the fixture are all
// present, and returns the absolute fixture path.
func requireTools(t *testing.T) string {
	t.Helper()
	for _, bin := range []string{"ffmpeg", "vmaf"} {
		if _, err := exec.LookPath(bin); err != nil {
			t.Skipf("%s not on PATH; skipping the pipeline integration test", bin)
		}
	}
	abs, err := filepath.Abs(fixtureRel)
	if err != nil {
		t.Fatalf("resolve fixture path: %v", err)
	}
	if _, err := os.Stat(abs); err != nil {
		t.Skipf("fixture %s unavailable: %v", abs, err)
	}
	return abs
}

// testConfig builds a PipelineConfig over the in-tree fixture.
func testConfig(t *testing.T, src string, proxy Proxy) PipelineConfig {
	t.Helper()
	return PipelineConfig{
		Src:                src,
		Width:              fixtureWidth,
		Height:             fixtureHeight,
		PixFmt:             "yuv420p",
		Framerate:          fixtureFPS,
		Encoder:            "libx264",
		Preset:             "ultrafast",
		CRFLo:              DefaultCRFLo,
		CRFHi:              DefaultCRFHi,
		SampleChunkSeconds: 1.0,
		FFmpegBin:          "ffmpeg",
		VMAFBin:            "vmaf",
		VMAFModel:          "vmaf_v0.6.1",
		ScoreBackend:       "cpu",
		EncodeDir:          t.TempDir(),
		Proxy:              proxy,
	}
}

// TestProbePipelineExtractsRealFeatures drives one probe end to end: encode a
// slice with ffmpeg, decode it back to raw YUV, score it with libvmaf, and
// parse the canonical-6 pooled means.
//
// This is the regression guard for the two silent-zero bugs the Python fast
// path carries on its probe leg:
//
//  1. cli._build_fast_sample_extractor hands the .mp4 straight to the libvmaf
//     CLI, which only reads raw YUV, so the score always fails;
//  2. cli._parse_canonical6_means looks up bare "adm2" / "vif_scale0" keys,
//     while libvmaf emits "integer_adm2" / "integer_vif_scale0".
//
// Either one on its own turns every probe's feature vector into six zeros. A
// non-zero, in-range vector here proves both are fixed.
func TestProbePipelineExtractsRealFeatures(t *testing.T) {
	src := requireTools(t)

	var (
		gotFeatures []float64
		gotEncoder  string
		gotCRFNorm  float64
	)
	proxy := ProxyFunc(func(_ context.Context, features []float64, encoder string, _, crfNorm float64) (float64, error) {
		gotFeatures = append([]float64(nil), features...)
		gotEncoder = encoder
		gotCRFNorm = crfNorm
		// A stand-in for the ONNX proxy: a crude monotone map so the value
		// is deterministic and in the VMAF range.
		return 50.0 + 50.0*(1.0-crfNorm), nil
	})

	cfg := testConfig(t, src, proxy)
	predict, err := NewSamplePredictor(context.Background(), cfg)
	if err != nil {
		t.Fatalf("NewSamplePredictor: %v", err)
	}

	const crf = 28
	sample, err := predict(crf)
	if err != nil {
		t.Fatalf("probe at CRF %d: %v", crf, err)
	}

	if sample.CRF != crf {
		t.Errorf("CRF = %d, want %d", sample.CRF, crf)
	}
	if gotEncoder != "libx264" {
		t.Errorf("proxy saw encoder %q, want libx264", gotEncoder)
	}
	wantNorm := cfg.crfNorm(crf)
	if math.Abs(gotCRFNorm-wantNorm) > 1e-12 {
		t.Errorf("proxy saw crf_norm %v, want %v", gotCRFNorm, wantNorm)
	}

	// The probe must have produced a real bitrate.
	if sample.PredictedKbps <= 0 {
		t.Errorf("observed bitrate = %v, want > 0 — the probe encode produced nothing",
			sample.PredictedKbps)
	}

	// The canonical-6 vector must not be the all-zero degradation.
	if len(gotFeatures) != len(canonical6) {
		t.Fatalf("proxy saw %d features, want %d", len(gotFeatures), len(canonical6))
	}
	allZero := true
	for i, v := range gotFeatures {
		if v != 0 {
			allZero = false
		}
		if math.IsNaN(v) || math.IsInf(v, 0) {
			t.Errorf("feature %s = %v, want a finite value", canonical6[i], v)
		}
	}
	if allZero {
		t.Fatalf("canonical-6 vector is all zeros (%v) — the probe score or the "+
			"pooled-key lookup silently failed", gotFeatures)
	}

	// adm2 and the vif scales are ratios in [0, 1]; motion2 is unbounded but
	// non-negative. A vector outside those ranges means the pooled keys were
	// mismatched rather than merely missing.
	for i := 0; i < 5; i++ {
		if gotFeatures[i] < 0 || gotFeatures[i] > 1.0001 {
			t.Errorf("feature %s = %v, outside the expected [0, 1] range",
				canonical6[i], gotFeatures[i])
		}
	}
	if gotFeatures[5] < 0 {
		t.Errorf("motion2 = %v, want >= 0", gotFeatures[5])
	}
	t.Logf("canonical-6 at CRF %d: %v (%.1f kbps)", crf, gotFeatures, sample.PredictedKbps)
}

// TestVerifyPipelineScoresRealEncode drives the mandatory verify pass end to
// end and checks it returns a plausible VMAF score for the full clip.
func TestVerifyPipelineScoresRealEncode(t *testing.T) {
	src := requireTools(t)

	cfg := testConfig(t, src, nil)
	verify, err := NewVerifier(cfg)
	if err != nil {
		t.Fatalf("NewVerifier: %v", err)
	}

	score, err := verify(context.Background(), "libx264", 30)
	if err != nil {
		t.Fatalf("verify pass: %v", err)
	}
	if math.IsNaN(score) || score <= 0 || score > 100 {
		t.Errorf("verify VMAF = %v, want a score in (0, 100]", score)
	}
	t.Logf("verify VMAF at CRF 30: %.4f", score)
}

// TestRecommendProductionEndToEnd runs the whole production flow — TPE over
// real probe encodes plus the mandatory verify pass — with the ONNX proxy
// stubbed. It is the closest the Go tree can get to a real `fast` run today,
// and it pins the payload the CLI would emit.
func TestRecommendProductionEndToEnd(t *testing.T) {
	src := requireTools(t)

	// A stub proxy standing in for fr_regressor_v2: a monotone CRF->VMAF map
	// that also consumes the real extracted features, so a regression in the
	// extraction path still surfaces here.
	proxy := ProxyFunc(func(_ context.Context, features []float64, _ string, _, crfNorm float64) (float64, error) {
		if len(features) != len(canonical6) {
			t.Errorf("proxy saw %d features, want %d", len(features), len(canonical6))
		}
		return 100.0 - 45.0*crfNorm, nil
	})

	cfg := testConfig(t, src, proxy)
	predict, err := NewSamplePredictor(context.Background(), cfg)
	if err != nil {
		t.Fatalf("NewSamplePredictor: %v", err)
	}
	verify, err := NewVerifier(cfg)
	if err != nil {
		t.Fatalf("NewVerifier: %v", err)
	}

	got, err := Recommend(context.Background(), Params{
		Src:        src,
		TargetVMAF: 92.0,
		Encoder:    cfg.Encoder,
		CRFLo:      cfg.CRFLo,
		CRFHi:      cfg.CRFHi,
		// A small budget: each trial is a real ffmpeg encode plus a libvmaf
		// score, so this test trades search quality for wall time.
		NTrials: 4,
		Predict: predict,
		Verify:  verify,
	})
	if err != nil {
		t.Fatalf("Recommend: %v", err)
	}

	if got.Smoke {
		t.Error("smoke = true in a production run")
	}
	if got.NTrials != 4 {
		t.Errorf("n_trials = %d, want 4", got.NTrials)
	}
	if got.RecommendedCRF < cfg.CRFLo || got.RecommendedCRF > cfg.CRFHi {
		t.Errorf("recommended_crf = %d, outside [%d, %d]",
			got.RecommendedCRF, cfg.CRFLo, cfg.CRFHi)
	}
	if got.VerifyVMAF == nil {
		t.Fatal("verify_vmaf is null — ADR-0304 requires the verify pass to run")
	}
	if v := *got.VerifyVMAF; math.IsNaN(v) || v <= 0 || v > 100 {
		t.Errorf("verify_vmaf = %v, want a score in (0, 100]", v)
	}
	if got.ProxyVerifyGap == nil {
		t.Fatal("proxy_verify_gap is null in a production run")
	}
	wantGap := math.Abs(got.PredictedVMAF - *got.VerifyVMAF)
	if math.Abs(*got.ProxyVerifyGap-wantGap) > 1e-9 {
		t.Errorf("proxy_verify_gap = %v, want |%v - %v| = %v",
			*got.ProxyVerifyGap, got.PredictedVMAF, *got.VerifyVMAF, wantGap)
	}
	t.Logf("production recommendation: CRF %d, proxy %.3f, verify %.3f, gap %.3f",
		got.RecommendedCRF, got.PredictedVMAF, *got.VerifyVMAF, *got.ProxyVerifyGap)
}
