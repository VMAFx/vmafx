// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package sidecar

import (
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/tune/predictor"
)

// ---------------------------------------------------------------------------
// Python parity.
// ---------------------------------------------------------------------------

// pythonSidecarFixture mirrors testdata/python_sidecar.json, dumped from
// vmaftune.sidecar.
type pythonSidecarFixture struct {
	LambdaL2       float64 `json:"lambda_l2"`
	MaxHistoryRows int     `json:"max_history_rows"`
	Captures       []struct {
		Features             map[string]float64 `json:"features"`
		Codec                string             `json:"codec"`
		CRF                  int                `json:"crf"`
		ObservedVMAF         float64            `json:"observed_vmaf"`
		PredictedVMAFBits    string             `json:"predicted_vmaf_bits"`
		CorrectionAfterBits  string             `json:"correction_after_bits"`
		NUpdatesAfter        int                `json:"n_updates_after"`
		ResidualRMSAfterBits string             `json:"residual_rms_after_bits"`
		WeightsAfterBits     []string           `json:"weights_after_bits"`
	} `json:"captures"`
	FinalState map[string]any `json:"final_state"`
}

func loadSidecarFixture(t *testing.T) pythonSidecarFixture {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("testdata", "python_sidecar.json"))
	if err != nil {
		t.Fatalf("read sidecar fixture: %v", err)
	}
	var fixture pythonSidecarFixture
	if err := json.Unmarshal(raw, &fixture); err != nil {
		t.Fatalf("parse sidecar fixture: %v", err)
	}
	if len(fixture.Captures) == 0 {
		t.Fatal("sidecar fixture is empty")
	}
	return fixture
}

func featuresFromMap(m map[string]float64) predictor.ShotFeatures {
	return predictor.ShotFeatures{
		ProbeBitrateKbps:    m["probe_bitrate_kbps"],
		ProbeIFrameAvgBytes: m["probe_i_frame_avg_bytes"],
		ProbePFrameAvgBytes: m["probe_p_frame_avg_bytes"],
		ProbeBFrameAvgBytes: m["probe_b_frame_avg_bytes"],
		SaliencyMean:        m["saliency_mean"],
		SaliencyVar:         m["saliency_var"],
		FrameDiffMean:       m["frame_diff_mean"],
		YAvg:                m["y_avg"],
		YVar:                m["y_var"],
		ShotLengthFrames:    int(m["shot_length_frames"]),
		FPS:                 m["fps"],
		Width:               int(m["width"]),
		Height:              int(m["height"]),
	}
}

func float64FromHex(t *testing.T, s string) float64 {
	t.Helper()
	raw, err := hex.DecodeString(s)
	if err != nil || len(raw) != 8 {
		t.Fatalf("bad hex float %q: %v", s, err)
	}
	var bits uint64
	for _, b := range raw {
		bits = bits<<8 | uint64(b)
	}
	return math.Float64frombits(bits)
}

// TestRidgeUpdateMatchesPython replays 40 captures through the online ridge
// fit and checks the weight vector, the correction, and the drift signal after
// *every* one.
//
// This is the load-bearing test for the sidecar port: the Sherman-Morrison
// rank-1 update is order-sensitive and self-referential, so a transposed index
// or a reordered operation would drift silently rather than fail loudly. The
// tolerance is bit-exact because both implementations do the same operations
// in the same order on IEEE-754 doubles.
func TestRidgeUpdateMatchesPython(t *testing.T) {
	t.Parallel()

	fixture := loadSidecarFixture(t)
	cfg := Config{
		CacheDir:       t.TempDir(),
		LambdaL2:       fixture.LambdaL2,
		MaxHistoryRows: fixture.MaxHistoryRows,
	}
	model := NewModel(cfg)
	base := &predictor.Predictor{}

	for i, capture := range fixture.Captures {
		features := featuresFromMap(capture.Features)

		wantPredicted := float64FromHex(t, capture.PredictedVMAFBits)
		gotPredicted := base.PredictVMAF(features, capture.CRF, capture.Codec)
		if math.Float64bits(gotPredicted) != math.Float64bits(wantPredicted) {
			t.Fatalf("capture %d: base prediction = %v, want %v (the fixture's "+
				"residuals are computed against this value)", i, gotPredicted, wantPredicted)
		}

		if err := model.Update(features, capture.ObservedVMAF, gotPredicted, capture.CRF); err != nil {
			t.Fatalf("capture %d: Update: %v", i, err)
		}

		if model.NUpdates != capture.NUpdatesAfter {
			t.Errorf("capture %d: NUpdates = %d, want %d", i, model.NUpdates, capture.NUpdatesAfter)
		}
		for j, wantBits := range capture.WeightsAfterBits {
			want := float64FromHex(t, wantBits)
			if math.Float64bits(model.Weights[j]) != math.Float64bits(want) {
				t.Fatalf("capture %d: weight[%d] = %v (%016x), want %v (%016x)",
					i, j, model.Weights[j], math.Float64bits(model.Weights[j]),
					want, math.Float64bits(want))
			}
		}
		wantCorrection := float64FromHex(t, capture.CorrectionAfterBits)
		gotCorrection := model.PredictCorrection(features, capture.CRF)
		if math.Float64bits(gotCorrection) != math.Float64bits(wantCorrection) {
			t.Errorf("capture %d: correction = %v, want %v", i, gotCorrection, wantCorrection)
		}
		wantRMS := float64FromHex(t, capture.ResidualRMSAfterBits)
		if math.Float64bits(model.RecentResidualRMS()) != math.Float64bits(wantRMS) {
			t.Errorf("capture %d: residual RMS = %v, want %v",
				i, model.RecentResidualRMS(), wantRMS)
		}
	}
}

// TestHistoryRingBufferIsBounded pins the MaxHistoryRows contract: the buffer
// keeps the most recent rows and drops the oldest, so the drift signal tracks
// recent behaviour rather than the whole history.
func TestHistoryRingBufferIsBounded(t *testing.T) {
	t.Parallel()

	cfg := Config{CacheDir: t.TempDir(), LambdaL2: 1.0, MaxHistoryRows: 3}
	model := NewModel(cfg)
	features := predictor.ShotFeatures{ProbeBitrateKbps: 1000}

	for i := 1; i <= 10; i++ {
		if err := model.Update(features, float64(i), 0, 23); err != nil {
			t.Fatalf("Update: %v", err)
		}
	}
	if len(model.History) != 3 {
		t.Fatalf("history length = %d, want 3", len(model.History))
	}
	for i, want := range []float64{8, 9, 10} {
		if model.History[i].ObservedVMAF != want {
			t.Errorf("history[%d].observed = %v, want %v (the oldest rows must drop)",
				i, model.History[i].ObservedVMAF, want)
		}
	}
	if model.NUpdates != 10 {
		t.Errorf("NUpdates = %d, want 10 — trimming history must not touch the counter",
			model.NUpdates)
	}
}

// TestColdStartCorrectionIsExactlyZero pins the degeneracy contract: with zero
// weights, SidecarPredictor must return the bare predictor's value untouched.
func TestColdStartCorrectionIsExactlyZero(t *testing.T) {
	t.Parallel()

	cfg := Config{CacheDir: t.TempDir()}
	model := NewModel(cfg)
	features := predictor.ShotFeatures{
		ProbeBitrateKbps: 4200.5, FPS: 24, Width: 1920, Height: 1080, ShotLengthFrames: 240,
	}
	if got := model.PredictCorrection(features, 26); got != 0.0 {
		t.Errorf("cold-start correction = %v, want exactly 0.0", got)
	}
	if got := model.RecentResidualRMS(); got != 0.0 {
		t.Errorf("cold-start residual RMS = %v, want exactly 0.0", got)
	}

	sp := &Predictor{Base: &predictor.Predictor{}, Model: model, Codec: "libx264"}
	bare := sp.Base.PredictVMAF(features, 26, "libx264")
	if got := sp.PredictVMAF(features, 26, ""); got != bare {
		t.Errorf("cold-start sidecar = %v, want the bare predictor's %v", got, bare)
	}
}

// TestUpdateSkipsPathologicalFeatures pins the numerical guard: a NaN or Inf
// feature must leave the state untouched rather than poisoning every future
// prediction.
func TestUpdateSkipsPathologicalFeatures(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		value float64
	}{
		{"NaN probe bitrate", math.NaN()},
		{"positive infinity", math.Inf(1)},
		{"negative infinity", math.Inf(-1)},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			model := NewModel(Config{CacheDir: t.TempDir()})
			features := predictor.ShotFeatures{ProbeBitrateKbps: tc.value}
			if err := model.Update(features, 90, 85, 23); err != nil {
				t.Fatalf("Update: %v", err)
			}
			if model.NUpdates != 0 {
				t.Errorf("NUpdates = %d, want 0 — the update should have been skipped",
					model.NUpdates)
			}
			for i, w := range model.Weights {
				if w != 0.0 {
					t.Errorf("weight[%d] = %v, want 0 — state must be untouched", i, w)
				}
			}
		})
	}
}

// TestPredictVMAFClampsRunawayCorrection guards the [0, 100] contract against
// a correction large enough to push the score out of the VMAF range.
func TestPredictVMAFClampsRunawayCorrection(t *testing.T) {
	t.Parallel()

	features := predictor.ShotFeatures{ProbeBitrateKbps: 1000}
	tests := []struct {
		name string
		bias float64
		want float64
	}{
		{"huge positive correction clamps at 100", 1e6, 100},
		{"huge negative correction clamps at 0", -1e6, 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			model := NewModel(Config{CacheDir: t.TempDir()})
			model.Weights[0] = tc.bias // the bias/intercept column
			sp := &Predictor{Base: &predictor.Predictor{}, Model: model, Codec: "libx264"}
			if got := sp.PredictVMAF(features, 23, ""); got != tc.want {
				t.Errorf("PredictVMAF = %v, want %v", got, tc.want)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Persistence.
// ---------------------------------------------------------------------------

// TestStateRoundTrip pins the save/load cycle: a reloaded model must be
// bit-identical, so an operator's fit survives a process restart.
func TestStateRoundTrip(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	cfg := Config{CacheDir: dir, LambdaL2: 1.0, MaxHistoryRows: 500,
		PredictorVersion: DefaultPredictorVersion}
	model := NewModel(cfg)
	features := predictor.ShotFeatures{ProbeBitrateKbps: 4200.5, FPS: 24, Width: 1920}
	for i := 0; i < 5; i++ {
		if err := model.Update(features, 90+float64(i), 85, 23+i); err != nil {
			t.Fatalf("Update: %v", err)
		}
	}

	path := filepath.Join(dir, "state.json")
	if err := model.Save(path); err != nil {
		t.Fatalf("Save: %v", err)
	}
	reloaded := Load(path, cfg)

	if reloaded.NUpdates != model.NUpdates {
		t.Errorf("NUpdates = %d, want %d", reloaded.NUpdates, model.NUpdates)
	}
	for i, w := range model.Weights {
		if math.Float64bits(reloaded.Weights[i]) != math.Float64bits(w) {
			t.Errorf("weight[%d] = %v, want %v", i, reloaded.Weights[i], w)
		}
	}
	for i, row := range model.AInv {
		for j, v := range row {
			if math.Float64bits(reloaded.AInv[i][j]) != math.Float64bits(v) {
				t.Errorf("a_inv[%d][%d] = %v, want %v", i, j, reloaded.AInv[i][j], v)
			}
		}
	}
	if len(reloaded.History) != len(model.History) {
		t.Errorf("history length = %d, want %d", len(reloaded.History), len(model.History))
	}
}

// TestLoadFallsBackToColdStart pins every documented discard path. The
// predictor-version case is the load-bearing one: it is what makes a shipped
// model upgrade safe, because a stale correction can never be replayed against
// a refreshed predictor.
func TestLoadFallsBackToColdStart(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	cfg := Config{CacheDir: dir, LambdaL2: 1.0, MaxHistoryRows: 500,
		PredictorVersion: DefaultPredictorVersion}

	trained := NewModel(cfg)
	if err := trained.Update(
		predictor.ShotFeatures{ProbeBitrateKbps: 1000}, 95, 85, 23); err != nil {
		t.Fatalf("Update: %v", err)
	}
	good := trained.ToMap()

	mutate := func(edit func(map[string]any)) string {
		state := map[string]any{}
		for k, v := range good {
			state[k] = v
		}
		edit(state)
		raw, err := json.Marshal(state)
		if err != nil {
			t.Fatalf("marshal state: %v", err)
		}
		path := filepath.Join(t.TempDir(), "state.json")
		if err := os.WriteFile(path, raw, 0o600); err != nil {
			t.Fatalf("write state: %v", err)
		}
		return path
	}

	tests := []struct {
		name          string
		path          string
		wantColdStart bool
	}{
		{"missing file", filepath.Join(dir, "absent.json"), true},
		{"unchanged state loads", mutate(func(map[string]any) {}), false},
		{
			"schema version mismatch",
			mutate(func(s map[string]any) { s["schema_version"] = 99 }), true,
		},
		{
			"feature dim mismatch",
			mutate(func(s map[string]any) { s["feature_dim"] = 13 }), true,
		},
		{
			"predictor version mismatch",
			mutate(func(s map[string]any) { s["predictor_version"] = "predictor_v2" }), true,
		},
		{
			"truncated weights",
			mutate(func(s map[string]any) { s["weights"] = []float64{1, 2, 3} }), true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := Load(tc.path, cfg)
			isColdStart := got.NUpdates == 0
			if isColdStart != tc.wantColdStart {
				t.Errorf("cold start = %v, want %v (n_updates=%d)",
					isColdStart, tc.wantColdStart, got.NUpdates)
			}
			if len(got.Weights) != FeatureDim || len(got.AInv) != FeatureDim {
				t.Errorf("loaded model has the wrong shape: %d weights, %d rows",
					len(got.Weights), len(got.AInv))
			}
		})
	}
}

// TestLoadLeavesCorruptFileInPlace: the operator must be able to inspect a
// corrupt state file, so the loader never deletes or rewrites it.
func TestLoadLeavesCorruptFileInPlace(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	path := filepath.Join(dir, "state.json")
	corrupt := []byte("{not json at all")
	if err := os.WriteFile(path, corrupt, 0o600); err != nil {
		t.Fatalf("write corrupt state: %v", err)
	}

	model := Load(path, Config{CacheDir: dir})
	if model.NUpdates != 0 {
		t.Error("corrupt state should cold-start")
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if string(after) != string(corrupt) {
		t.Errorf("the corrupt file was modified: %q", after)
	}
}

// ---------------------------------------------------------------------------
// Host UUID.
// ---------------------------------------------------------------------------

var hexToken = regexp.MustCompile(`^[0-9a-f]{32}$`)

// TestHostUUIDIsStableRandomAndAnonymous pins all three properties the
// upload-consent story depends on: 128 bits of entropy, stable across calls,
// and not derived from anything machine-identifying.
func TestHostUUIDIsStableRandomAndAnonymous(t *testing.T) {
	t.Parallel()

	dirA, dirB := t.TempDir(), t.TempDir()

	first, err := GetOrCreateHostUUID(dirA)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID: %v", err)
	}
	if !hexToken.MatchString(first) {
		t.Errorf("uuid %q is not 32 lowercase hex characters", first)
	}

	again, err := GetOrCreateHostUUID(dirA)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID (repeat): %v", err)
	}
	if again != first {
		t.Errorf("uuid changed between calls: %q then %q", first, again)
	}

	// A second cache root must produce a different token. If the UUID were
	// derived from the machine (MAC, hostname, machine-id) these would match.
	other, err := GetOrCreateHostUUID(dirB)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID (other root): %v", err)
	}
	if other == first {
		t.Error("two independent cache roots produced the same uuid; " +
			"the token must be random, never machine-derived")
	}

	// The token lives above the per-predictor-version subdirectories so it
	// survives an upgrade.
	if got := HostUUIDPath(dirA); got != filepath.Join(dirA, "host-uuid") {
		t.Errorf("HostUUIDPath = %q, want the cache-dir root", got)
	}
}

// TestHostUUIDRegeneratesWhenBlank covers the empty-file recovery path.
func TestHostUUIDRegeneratesWhenBlank(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	if err := os.WriteFile(HostUUIDPath(dir), []byte("   \n"), 0o600); err != nil {
		t.Fatalf("write blank uuid: %v", err)
	}
	got, err := GetOrCreateHostUUID(dir)
	if err != nil {
		t.Fatalf("GetOrCreateHostUUID: %v", err)
	}
	if !hexToken.MatchString(got) {
		t.Errorf("uuid %q is not 32 lowercase hex characters", got)
	}
}

// ---------------------------------------------------------------------------
// Configuration and layout.
// ---------------------------------------------------------------------------

// TestStatePathLayout pins the on-disk layout the operator guide documents.
func TestStatePathLayout(t *testing.T) {
	t.Parallel()

	cfg := Config{CacheDir: "/cache", PredictorVersion: "predictor_v7"}
	want := filepath.Join("/cache", "predictor_v7", "libx265", "state.json")
	if got := StatePathFor(cfg, "libx265"); got != want {
		t.Errorf("StatePathFor = %q, want %q", got, want)
	}
}

// TestDefaultCacheDirHonoursXDG pins the freedesktop base-dir behaviour.
func TestDefaultCacheDirHonoursXDG(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", "/xdg-root")
	want := filepath.Join("/xdg-root", "vmaf-tune", "sidecar")
	if got := DefaultCacheDir(); got != want {
		t.Errorf("DefaultCacheDir = %q, want %q", got, want)
	}
}

func TestDefaultCacheDirFallsBackToHome(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", "")
	got := DefaultCacheDir()
	if !strings.HasSuffix(got, filepath.Join(".cache", "vmaf-tune", "sidecar")) {
		t.Errorf("DefaultCacheDir = %q, want a ~/.cache/vmaf-tune/sidecar suffix", got)
	}
}

// TestConfigNormaliseFillsZeroValues documents that a partially-filled Config
// picks up the shipped defaults rather than a lambda of 0 (which would make
// the cold-start inverse Gram infinite).
func TestConfigNormaliseFillsZeroValues(t *testing.T) {
	t.Parallel()

	got := Config{CacheDir: "/cache"}.normalise()
	if got.LambdaL2 != 1.0 {
		t.Errorf("LambdaL2 = %v, want 1.0", got.LambdaL2)
	}
	if got.MaxHistoryRows != 500 {
		t.Errorf("MaxHistoryRows = %d, want 500", got.MaxHistoryRows)
	}
	if got.PredictorVersion != DefaultPredictorVersion {
		t.Errorf("PredictorVersion = %q, want %q", got.PredictorVersion, DefaultPredictorVersion)
	}
	if got.CacheDir != "/cache" {
		t.Errorf("an explicit CacheDir must survive normalisation, got %q", got.CacheDir)
	}
}

// TestFeatureVectorLayout pins the column order, which is what every persisted
// weight vector is indexed by. Changing it silently mis-aligns saved state.
func TestFeatureVectorLayout(t *testing.T) {
	t.Parallel()

	features := predictor.ShotFeatures{
		ProbeBitrateKbps: 2, ProbeIFrameAvgBytes: 3, ProbePFrameAvgBytes: 4,
		ProbeBFrameAvgBytes: 5, SaliencyMean: 6, SaliencyVar: 7, FrameDiffMean: 8,
		YAvg: 9, YVar: 10, ShotLengthFrames: 11, FPS: 12, Width: 13, Height: 99,
	}
	got, err := FeatureVector(features, 1)
	if err != nil {
		t.Fatalf("FeatureVector: %v", err)
	}
	want := []float64{1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}
	if len(got) != FeatureDim {
		t.Fatalf("length = %d, want %d", len(got), FeatureDim)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("column %d = %v, want %v", i, got[i], want[i])
		}
	}
	// Height is deliberately absent: the vector is 14 wide and stops at Width.
	for _, v := range got {
		if v == 99 {
			t.Error("Height leaked into the feature vector; the layout is 14 columns " +
				"ending at Width")
		}
	}
}

// TestForCodecLoadsExistingState checks the wiring end to end: a sidecar built
// for a codec picks up the state a previous run persisted for that codec, and
// not one persisted for another.
func TestForCodecLoadsExistingState(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	cfg := Config{CacheDir: dir, PredictorVersion: DefaultPredictorVersion}
	base := &predictor.Predictor{}
	features := predictor.ShotFeatures{ProbeBitrateKbps: 4200.5, FPS: 24, Width: 1920}

	first, err := ForCodec(base, "libx264", cfg)
	if err != nil {
		t.Fatalf("ForCodec: %v", err)
	}
	if err := first.RecordCapture(features, 26, 91.5, "", true); err != nil {
		t.Fatalf("RecordCapture: %v", err)
	}

	reopened, err := ForCodec(base, "libx264", cfg)
	if err != nil {
		t.Fatalf("ForCodec (reopen): %v", err)
	}
	if reopened.Model.NUpdates != 1 {
		t.Errorf("reopened sidecar has %d updates, want 1", reopened.Model.NUpdates)
	}
	if reopened.HostUUID != first.HostUUID {
		t.Errorf("host uuid changed across reopen: %q then %q",
			first.HostUUID, reopened.HostUUID)
	}

	otherCodec, err := ForCodec(base, "libx265", cfg)
	if err != nil {
		t.Fatalf("ForCodec (other codec): %v", err)
	}
	if otherCodec.Model.NUpdates != 0 {
		t.Error("a different codec bucket must start cold")
	}
}

// TestRecordCaptureNoPersist covers the tests-only path that skips disk I/O.
func TestRecordCaptureNoPersist(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	cfg := Config{CacheDir: dir, PredictorVersion: DefaultPredictorVersion}
	sp, err := ForCodec(&predictor.Predictor{}, "libx264", cfg)
	if err != nil {
		t.Fatalf("ForCodec: %v", err)
	}
	features := predictor.ShotFeatures{ProbeBitrateKbps: 1000}
	if err := sp.RecordCapture(features, 23, 90, "", false); err != nil {
		t.Fatalf("RecordCapture: %v", err)
	}
	if sp.Model.NUpdates != 1 {
		t.Errorf("in-memory update did not land: %d", sp.Model.NUpdates)
	}
	// Carried over from the parallel pkg/sidecar port, which asserted the other
	// half of this contract: persist=false must leave nothing on disk, not just
	// update memory.
	if _, statErr := os.Stat(sp.StatePath); statErr == nil {
		t.Error("RecordCapture(persist=false) wrote a state file")
	}
	if _, err := os.Stat(sp.StatePath); !os.IsNotExist(err) {
		t.Errorf("no-persist should not have written %s (err=%v)", sp.StatePath, err)
	}
}
