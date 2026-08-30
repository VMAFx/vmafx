// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/sidecar/sidecar_test.go — online-ridge and persistence tests.
//
// The numeric expectations were produced by driving the Python
// vmaftune.sidecar.SidecarModel through the same capture sequence, so they pin
// cross-implementation agreement on the Sherman-Morrison update rather than
// restating the Go arithmetic. The on-disk state written here is byte-loadable
// by the Python implementation and vice versa — see the migration report for
// the end-to-end round-trip evidence.

package sidecar

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/predictor"
)

// testFeatures is the fixture the parity values were computed against.
func testFeatures() predictor.ShotFeatures {
	return predictor.ShotFeatures{
		ProbeBitrateKbps:    4200.5,
		ProbeIFrameAvgBytes: 51234.0,
		ProbePFrameAvgBytes: 8123.25,
		ProbeBFrameAvgBytes: 2011.75,
		SaliencyMean:        0.42,
		SaliencyVar:         0.031,
		FrameDiffMean:       7.5,
		YAvg:                112.25,
		YVar:                1830.5,
		ShotLengthFrames:    240,
		FPS:                 24.0,
		Width:               1920,
		Height:              1080,
	}
}

// testConfig pins the cache dir and host UUID so a test never touches the real
// XDG cache or spends entropy.
func testConfig(t *testing.T) Config {
	t.Helper()
	cfg := NewConfig()
	cfg.CacheDir = t.TempDir()
	cfg.HostUUID = "0123456789abcdef0123456789abcdef"
	return cfg
}

func TestFeatureVectorLayout(t *testing.T) {
	t.Parallel()

	// The column order is load-bearing: it pins which weight belongs to
	// which feature in every persisted state file.
	got := FeatureVector(testFeatures(), 26)
	want := []float64{
		1.0, 26, 4200.5, 51234.0, 8123.25, 2011.75,
		0.42, 0.031, 7.5, 112.25, 1830.5, 240, 24.0, 1920,
	}
	if len(got) != FeatureDim {
		t.Fatalf("FeatureVector length = %d, want FeatureDim = %d", len(got), FeatureDim)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("FeatureVector[%d] = %v, want %v", i, got[i], want[i])
		}
	}
}

func TestColdStartCorrectionIsExactlyZero(t *testing.T) {
	t.Parallel()

	m := NewModel(testConfig(t))
	if got := m.PredictCorrection(testFeatures(), 26); got != 0.0 {
		t.Errorf("cold-start PredictCorrection = %v, want exactly 0.0", got)
	}
	if got := m.RecentResidualRMS(); got != 0.0 {
		t.Errorf("cold-start RecentResidualRMS = %v, want 0.0", got)
	}
	if m.NUpdates != 0 {
		t.Errorf("cold-start NUpdates = %d, want 0", m.NUpdates)
	}
}

func TestUpdateMatchesPythonSidecar(t *testing.T) {
	t.Parallel()

	// One capture through the Python SidecarModel:
	//   observed 91.75, predicted 96.72695148356705 at crf 26.
	// The residual and the resulting correction are pinned below.
	m := NewModel(testConfig(t))
	f := testFeatures()
	const observed, predicted = 91.75, 96.72695148356705
	m.Update(f, observed, predicted, 26)

	if m.NUpdates != 1 {
		t.Fatalf("NUpdates = %d, want 1", m.NUpdates)
	}
	wantResidual := observed - predicted
	if got := m.RecentResidualRMS(); math.Abs(got-math.Abs(wantResidual)) > 1e-12 {
		t.Errorf("RecentResidualRMS after one capture = %v, want %v",
			got, math.Abs(wantResidual))
	}
	// The correction is tiny after one heavily-regularised capture: the
	// exact value is what the Python implementation reports for the same
	// input, and a drift here means the Sherman-Morrison update diverged.
	const wantCorrection = -1.8299570604290876e-09
	got := m.PredictCorrection(f, 26)
	if math.Abs(got-wantCorrection) > 1e-20 {
		t.Errorf("PredictCorrection after one capture = %v, want %v", got, wantCorrection)
	}
}

func TestUpdateSkipsNonFiniteFeatures(t *testing.T) {
	t.Parallel()

	m := NewModel(testConfig(t))
	f := testFeatures()
	f.YVar = math.NaN()
	m.Update(f, 91.0, 95.0, 26)

	if m.NUpdates != 0 {
		t.Errorf("a NaN feature was folded into the fit (NUpdates = %d)", m.NUpdates)
	}
	for i, w := range m.Weights {
		if w != 0.0 {
			t.Fatalf("weight[%d] = %v after a skipped update, want 0.0", i, w)
		}
	}
}

func TestHistoryRingBufferIsBounded(t *testing.T) {
	t.Parallel()

	cfg := testConfig(t)
	cfg.MaxHistoryRows = 3
	m := NewModel(cfg)
	f := testFeatures()
	for i := 0; i < 10; i++ {
		m.Update(f, float64(90+i), 95.0, 26)
	}
	if len(m.History) != 3 {
		t.Errorf("history length = %d, want the configured cap of 3", len(m.History))
	}
	if m.NUpdates != 10 {
		t.Errorf("NUpdates = %d, want 10 — the cap bounds history, not the fit", m.NUpdates)
	}
	// The buffer must keep the newest rows, not the oldest.
	if last := m.History[len(m.History)-1]; last.ObservedVMAF != 99.0 {
		t.Errorf("newest history row observed = %v, want 99.0", last.ObservedVMAF)
	}
}

func TestSaveLoadRoundTrip(t *testing.T) {
	t.Parallel()

	cfg := testConfig(t)
	m := NewModel(cfg)
	f := testFeatures()
	m.Update(f, 91.75, 96.72695148356705, 26)
	m.Update(f, 88.0, 92.0, 32)

	path := filepath.Join(cfg.CacheDir, "state.json")
	if err := m.Save(path); err != nil {
		t.Fatalf("Save: %v", err)
	}

	loaded := Load(path, cfg)
	if loaded.NUpdates != m.NUpdates {
		t.Errorf("round-tripped NUpdates = %d, want %d", loaded.NUpdates, m.NUpdates)
	}
	if got, want := loaded.PredictCorrection(f, 26), m.PredictCorrection(f, 26); got != want {
		t.Errorf("round-tripped correction = %v, want %v (bit-identical)", got, want)
	}
	if len(loaded.History) != len(m.History) {
		t.Errorf("round-tripped history length = %d, want %d",
			len(loaded.History), len(m.History))
	}
}

func TestLoadFallsBackToColdStart(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	cfg := testConfig(t)
	cfg.CacheDir = dir

	tests := []struct {
		name    string
		content string
	}{
		{name: "missing file", content: ""},
		{name: "corrupt json", content: "{not json"},
		{name: "wrong schema version", content: `{"schema_version": 999, "feature_dim": 14,
			"predictor_version": "predictor_v1", "weights": [], "a_inv": []}`},
		{name: "wrong feature dim", content: `{"schema_version": 1, "feature_dim": 7,
			"predictor_version": "predictor_v1", "weights": [], "a_inv": []}`},
		{
			// The predictor-version guard is what makes a shipped-model
			// upgrade safe: a stale correction must never be replayed.
			name: "predictor version mismatch",
			content: `{"schema_version": 1, "feature_dim": 14,
			"predictor_version": "predictor_v0", "weights": [], "a_inv": []}`,
		},
		{name: "wrong shape", content: `{"schema_version": 1, "feature_dim": 14,
			"predictor_version": "predictor_v1", "weights": [1.0], "a_inv": []}`},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "state.json")
			if tc.content != "" {
				if err := os.WriteFile(path, []byte(tc.content), 0o600); err != nil {
					t.Fatalf("seed state: %v", err)
				}
			}
			m := Load(path, cfg)
			if m.NUpdates != 0 {
				t.Errorf("Load returned a warm model (NUpdates = %d), want cold start",
					m.NUpdates)
			}
			if got := m.PredictCorrection(testFeatures(), 26); got != 0.0 {
				t.Errorf("cold-start correction = %v, want 0.0", got)
			}
			if tc.content != "" {
				// The corrupted file is left in place for inspection.
				if _, err := os.Stat(path); err != nil {
					t.Errorf("Load removed the unreadable state file: %v", err)
				}
			}
		})
	}
}

func TestSavedStateShape(t *testing.T) {
	t.Parallel()

	cfg := testConfig(t)
	m := NewModel(cfg)
	m.Update(testFeatures(), 91.0, 95.0, 26)
	path := filepath.Join(cfg.CacheDir, "state.json")
	if err := m.Save(path); err != nil {
		t.Fatalf("Save: %v", err)
	}

	data, err := os.ReadFile(path) //nolint:gosec // test-owned temp path
	if err != nil {
		t.Fatalf("read state: %v", err)
	}
	var raw map[string]any
	if uErr := json.Unmarshal(data, &raw); uErr != nil {
		t.Fatalf("state is not valid JSON: %v", uErr)
	}
	// These are the keys vmaftune.sidecar.SidecarModel.from_dict reads; a
	// missing one makes the state unloadable by the Python implementation.
	for _, key := range []string{
		"schema_version", "predictor_version", "feature_dim", "lambda_l2",
		"weights", "a_inv", "history", "n_updates",
	} {
		if _, ok := raw[key]; !ok {
			t.Errorf("saved state is missing key %q", key)
		}
	}
	if got := raw["feature_dim"]; got != float64(FeatureDim) {
		t.Errorf("saved feature_dim = %v, want %d", got, FeatureDim)
	}
}

func TestGetOrCreateHostUUID(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	first, err := GetOrCreateHostUUID(dir)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID: %v", err)
	}
	if len(first) != 32 {
		t.Errorf("host uuid %q has length %d, want 32 hex chars", first, len(first))
	}
	second, err := GetOrCreateHostUUID(dir)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID (second call): %v", err)
	}
	if second != first {
		t.Errorf("host uuid is not stable across calls: %q then %q", first, second)
	}

	// A different install gets a different token — the UUID must carry no
	// machine-identifying signal.
	other, err := GetOrCreateHostUUID(t.TempDir())
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID (other dir): %v", err)
	}
	if other == first {
		t.Error("two independent installs produced the same host uuid")
	}
}

func TestHostUUIDSurvivesEmptyFile(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "host-uuid"), []byte("  \n"), 0o600); err != nil {
		t.Fatalf("seed empty uuid: %v", err)
	}
	uuid, err := GetOrCreateHostUUID(dir)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID: %v", err)
	}
	if len(uuid) != 32 {
		t.Errorf("an empty uuid file should be regenerated, got %q", uuid)
	}
}

func TestStatePathLayout(t *testing.T) {
	t.Parallel()

	cfg := testConfig(t)
	got := StatePathFor(cfg, "libx265")
	want := filepath.Join(cfg.CacheDir, "predictor_v1", "libx265", "state.json")
	if got != want {
		t.Errorf("StatePathFor = %q, want %q", got, want)
	}
}

func TestPredictorCompositionAndClamp(t *testing.T) {
	t.Parallel()

	cfg := testConfig(t)
	sp, err := ForCodec(predictor.New(), "libx264", cfg)
	if err != nil {
		t.Fatalf("ForCodec: %v", err)
	}
	f := testFeatures()

	// Cold start: the composed prediction equals the bare predictor's.
	base := sp.Base.PredictVMAF(f, 26, "libx264")
	if got := sp.PredictVMAF(f, 26, ""); got != base {
		t.Errorf("cold-start composed prediction = %v, want the bare %v", got, base)
	}

	// A runaway correction must not push the score out of [0, 100].
	for i := range sp.Model.Weights {
		sp.Model.Weights[i] = 1e6
	}
	if got := sp.PredictVMAF(f, 26, ""); got != 100.0 {
		t.Errorf("a huge positive correction produced %v, want the 100.0 clamp", got)
	}
	for i := range sp.Model.Weights {
		sp.Model.Weights[i] = -1e6
	}
	if got := sp.PredictVMAF(f, 26, ""); got != 0.0 {
		t.Errorf("a huge negative correction produced %v, want the 0.0 clamp", got)
	}
}

func TestRecordCapturePersistence(t *testing.T) {
	t.Parallel()

	cfg := testConfig(t)
	sp, err := ForCodec(predictor.New(), "libx264", cfg)
	if err != nil {
		t.Fatalf("ForCodec: %v", err)
	}
	f := testFeatures()

	// persist=false leaves nothing on disk.
	if rErr := sp.RecordCapture(f, 26, 91.75, "", false); rErr != nil {
		t.Fatalf("RecordCapture: %v", rErr)
	}
	if _, statErr := os.Stat(sp.StatePath); statErr == nil {
		t.Error("RecordCapture(persist=false) wrote a state file")
	}

	// persist=true writes it, and a fresh sidecar reads the fit back.
	if rErr := sp.RecordCapture(f, 32, 88.0, "", true); rErr != nil {
		t.Fatalf("RecordCapture: %v", rErr)
	}
	reloaded, err := ForCodec(predictor.New(), "libx264", cfg)
	if err != nil {
		t.Fatalf("ForCodec (reload): %v", err)
	}
	if reloaded.Model.NUpdates != 2 {
		t.Errorf("reloaded NUpdates = %d, want 2", reloaded.Model.NUpdates)
	}
	if got, want := reloaded.Model.PredictCorrection(f, 26),
		sp.Model.PredictCorrection(f, 26); got != want {
		t.Errorf("reloaded correction = %v, want %v (bit-identical)", got, want)
	}
}

func TestConfigDefaults(t *testing.T) {
	t.Parallel()

	cfg := NewConfig()
	if cfg.LambdaL2 != 1.0 {
		t.Errorf("default LambdaL2 = %v, want 1.0", cfg.LambdaL2)
	}
	if cfg.MaxHistoryRows != 500 {
		t.Errorf("default MaxHistoryRows = %d, want 500", cfg.MaxHistoryRows)
	}
	if cfg.PredictorVersion != DefaultPredictorVersion {
		t.Errorf("default PredictorVersion = %q, want %q",
			cfg.PredictorVersion, DefaultPredictorVersion)
	}
	if cfg.TrainingCadence != "per-capture" {
		t.Errorf("default TrainingCadence = %q, want per-capture", cfg.TrainingCadence)
	}
}

func TestDefaultCacheDirHonoursXDG(t *testing.T) {
	// Not parallel: it mutates the process environment.
	t.Setenv("XDG_CACHE_HOME", "/xdg/cache")
	want := filepath.Join("/xdg/cache", "vmaf-tune", "sidecar")
	if got := DefaultCacheDir(); got != want {
		t.Errorf("DefaultCacheDir with XDG_CACHE_HOME = %q, want %q", got, want)
	}

	t.Setenv("XDG_CACHE_HOME", "")
	got := DefaultCacheDir()
	if filepath.Base(got) != "sidecar" || filepath.Base(filepath.Dir(got)) != "vmaf-tune" {
		t.Errorf("DefaultCacheDir without XDG_CACHE_HOME = %q, want .../vmaf-tune/sidecar", got)
	}
}
