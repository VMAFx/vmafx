// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/fast/proxy_test.go — table-driven tests for the fr_regressor_v2 proxy
// seam ported from tools/vmaf-tune/src/vmaftune/proxy.py.

package fast

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// writeProxyFixture materialises a <id>.onnx / <id>.json pair in a temp dir
// and returns the directory. The .onnx content is irrelevant — nothing in
// this package parses the graph; only its presence is checked.
func writeProxyFixture(t *testing.T, id string, sidecar ProxySidecar) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, id+".onnx"), []byte("onnx"), 0o600); err != nil {
		t.Fatalf("write onnx fixture: %v", err)
	}
	raw, err := json.Marshal(sidecar)
	if err != nil {
		t.Fatalf("marshal sidecar: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, id+".json"), raw, 0o600); err != nil {
		t.Fatalf("write sidecar fixture: %v", err)
	}
	return dir
}

// shippedVocab is the encoder vocabulary in model/tiny/fr_regressor_v2.json,
// which is also ai/scripts/train_fr_regressor_v2.py's ENCODER_VOCAB. Note it
// is NOT the tuple hardcoded in vmaftune/proxy.py — see ProxyModel.CodecBlock.
var shippedVocab = []string{
	"libx264", "libx265", "libsvtav1", "libvvenc", "libvpx-vp9",
	"h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_qsv", "hevc_qsv",
	"av1_qsv", "unknown",
}

// shippedSidecar mirrors the real model/tiny/fr_regressor_v2.json contract.
func shippedSidecar() ProxySidecar {
	return ProxySidecar{
		ID:           DefaultProxyModelID,
		ONNX:         DefaultProxyModelID + ".onnx",
		InputNames:   []string{"features", "codec"},
		OutputNames:  []string{"score"},
		EncoderVocab: shippedVocab,
		FeatureOrder: canonical6,
		FeatureMean: []float64{
			0.9550306797027588, 0.5178422331809998, 0.8622183203697205,
			0.9155327677726746, 0.9446256160736084, 8.946569442749023,
		},
		FeatureStd: []float64{
			0.03405159339308739, 0.15771430730819702, 0.11909843236207962,
			0.08633148670196533, 0.06272486597299576, 4.331558704376221,
		},
	}
}

// TestLoadProxyModel covers resolution and sidecar parsing.
func TestLoadProxyModel(t *testing.T) {
	t.Parallel()

	dir := writeProxyFixture(t, DefaultProxyModelID, shippedSidecar())

	got, err := LoadProxyModel(dir, DefaultProxyModelID)
	if err != nil {
		t.Fatalf("LoadProxyModel: %v", err)
	}
	if got.ModelID != DefaultProxyModelID {
		t.Errorf("ModelID = %q, want %q", got.ModelID, DefaultProxyModelID)
	}
	if got.ONNXPath != filepath.Join(dir, DefaultProxyModelID+".onnx") {
		t.Errorf("ONNXPath = %q", got.ONNXPath)
	}
	if len(got.Sidecar.EncoderVocab) != 12 {
		t.Errorf("encoder_vocab length = %d, want 12", len(got.Sidecar.EncoderVocab))
	}
	if len(got.Sidecar.InputNames) != 2 {
		t.Errorf("input_names = %v, want two ports", got.Sidecar.InputNames)
	}

	// An empty model id defaults to fr_regressor_v2.
	byDefault, err := LoadProxyModel(dir, "")
	if err != nil {
		t.Fatalf("LoadProxyModel with an empty id: %v", err)
	}
	if byDefault.ModelID != DefaultProxyModelID {
		t.Errorf("default ModelID = %q, want %q", byDefault.ModelID, DefaultProxyModelID)
	}
}

// TestLoadProxyModelErrors covers the not-found and malformed paths.
func TestLoadProxyModelErrors(t *testing.T) {
	t.Parallel()

	t.Run("missing onnx", func(t *testing.T) {
		t.Parallel()
		_, err := LoadProxyModel(t.TempDir(), DefaultProxyModelID)
		if !errors.Is(err, ErrProxyModelNotFound) {
			t.Fatalf("want ErrProxyModelNotFound, got %v", err)
		}
	})

	t.Run("missing sidecar", func(t *testing.T) {
		t.Parallel()
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "m.onnx"), []byte("onnx"), 0o600); err != nil {
			t.Fatalf("write onnx: %v", err)
		}
		_, err := LoadProxyModel(dir, "m")
		if !errors.Is(err, ErrProxyModelNotFound) {
			t.Fatalf("want ErrProxyModelNotFound, got %v", err)
		}
	})

	t.Run("malformed sidecar", func(t *testing.T) {
		t.Parallel()
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "m.onnx"), []byte("onnx"), 0o600); err != nil {
			t.Fatalf("write onnx: %v", err)
		}
		if err := os.WriteFile(filepath.Join(dir, "m.json"), []byte("{"), 0o600); err != nil {
			t.Fatalf("write sidecar: %v", err)
		}
		if _, err := LoadProxyModel(dir, "m"); err == nil {
			t.Fatal("want a parse error for a malformed sidecar, got nil")
		}
	})
}

// TestValidateModelID covers the registry-id guard that keeps a
// caller-supplied model id from escaping the resolved model directory.
func TestValidateModelID(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		id      string
		wantErr bool
	}{
		{name: "the shipped id", id: "fr_regressor_v2"},
		{name: "dashes and dots", id: "fr-regressor.v2"},
		{name: "digits", id: "model123"},
		{name: "empty is rejected", id: "", wantErr: true},
		{name: "a relative path is rejected", id: "../secrets", wantErr: true},
		{name: "an absolute path is rejected", id: "/etc/passwd", wantErr: true},
		{name: "a nested path is rejected", id: "sub/model", wantErr: true},
		{name: "dot is rejected", id: ".", wantErr: true},
		{name: "double dot is rejected", id: "..", wantErr: true},
		{name: "a space is rejected", id: "my model", wantErr: true},
		{name: "a null byte is rejected", id: "model\x00", wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			err := validateModelID(tc.id)
			if tc.wantErr && err == nil {
				t.Fatalf("validateModelID(%q): want an error, got nil", tc.id)
			}
			if !tc.wantErr && err != nil {
				t.Fatalf("validateModelID(%q): %v", tc.id, err)
			}
		})
	}
}

// TestLoadProxyModelRejectsPathIDs verifies the guard is wired into the
// public entry point, not just the helper.
func TestLoadProxyModelRejectsPathIDs(t *testing.T) {
	t.Parallel()

	dir := writeProxyFixture(t, DefaultProxyModelID, shippedSidecar())
	if _, err := LoadProxyModel(dir, "../escape"); err == nil {
		t.Fatal("want an error for a path-shaped model id, got nil")
	}
	if _, err := NewORTProxy(dir, "../escape"); err == nil {
		t.Fatal("NewORTProxy must reject a path-shaped model id too")
	}
}

// TestResolveProxyDirHonoursEnv checks VMAFX_MODEL_DIR resolution, including
// the model/tiny subdirectory layout the container ships.
func TestResolveProxyDirHonoursEnv(t *testing.T) {
	root := t.TempDir()
	tiny := filepath.Join(root, "tiny")
	if err := os.MkdirAll(tiny, 0o750); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	if err := os.WriteFile(filepath.Join(tiny, "m.onnx"), []byte("onnx"), 0o600); err != nil {
		t.Fatalf("write onnx: %v", err)
	}
	t.Setenv("VMAFX_MODEL_DIR", root)

	got, err := resolveProxyDir("", "m")
	if err != nil {
		t.Fatalf("resolveProxyDir: %v", err)
	}
	if got != tiny {
		t.Errorf("resolved dir = %q, want %q", got, tiny)
	}
}

// TestCodecBlock pins the one-hot layout against the shipped vocabulary.
//
// The Python vmaftune/proxy.py hardcodes a DIFFERENT ordering than the model
// was trained with; the cases below assert the sidecar-driven ordering that
// this port uses, and the two divergent slots are called out by name.
func TestCodecBlock(t *testing.T) {
	t.Parallel()

	dir := writeProxyFixture(t, DefaultProxyModelID, shippedSidecar())
	model, err := LoadProxyModel(dir, DefaultProxyModelID)
	if err != nil {
		t.Fatalf("LoadProxyModel: %v", err)
	}

	tests := []struct {
		name     string
		encoder  string
		wantSlot int
		wantErr  bool
	}{
		{name: "libx264 is slot 0", encoder: "libx264", wantSlot: 0},
		{name: "libx265 is slot 1", encoder: "libx265", wantSlot: 1},
		{name: "libsvtav1 is slot 2", encoder: "libsvtav1", wantSlot: 2},
		// proxy.py puts libaom-av1 here and libvvenc at 5; the trainer and
		// the shipped sidecar put libvvenc at 3.
		{name: "libvvenc is slot 3, not 5", encoder: "libvvenc", wantSlot: 3},
		{name: "libvpx-vp9 is slot 4", encoder: "libvpx-vp9", wantSlot: 4},
		{name: "h264_nvenc is slot 5, not 6", encoder: "h264_nvenc", wantSlot: 5},
		{name: "av1_qsv is the last codec slot", encoder: "av1_qsv", wantSlot: 10},
		{name: "unknown is the model's own catch-all", encoder: "unknown", wantSlot: 11},
		// proxy.py accepts libaom-av1 (and mis-slots it); the shipped model
		// never saw it, so it falls into the unknown slot here.
		{name: "libaom-av1 falls back to unknown", encoder: "libaom-av1", wantSlot: 11},
		{name: "a nonsense codec falls back to unknown", encoder: "not-a-codec", wantSlot: 11},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			block, err := model.CodecBlock(tc.encoder, 0.5, 0.25)
			if tc.wantErr {
				if err == nil {
					t.Fatal("want an error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("CodecBlock: %v", err)
			}
			if len(block) != len(shippedVocab)+2 {
				t.Fatalf("block length = %d, want %d", len(block), len(shippedVocab)+2)
			}
			for i := 0; i < len(shippedVocab); i++ {
				want := 0.0
				if i == tc.wantSlot {
					want = 1.0
				}
				if block[i] != want {
					t.Errorf("one-hot[%d] (%s) = %v, want %v",
						i, shippedVocab[i], block[i], want)
				}
			}
			if block[len(shippedVocab)] != 0.5 {
				t.Errorf("preset_norm = %v, want 0.5", block[len(shippedVocab)])
			}
			if block[len(shippedVocab)+1] != 0.25 {
				t.Errorf("crf_norm = %v, want 0.25", block[len(shippedVocab)+1])
			}
		})
	}
}

// TestCodecBlockWithoutUnknownSlot verifies the hard-error path: a model whose
// vocabulary has no catch-all must reject an out-of-vocabulary codec rather
// than shipping a silent zero-vector.
func TestCodecBlockWithoutUnknownSlot(t *testing.T) {
	t.Parallel()

	sidecar := shippedSidecar()
	sidecar.EncoderVocab = []string{"libx264", "libx265"}
	dir := writeProxyFixture(t, "narrow", sidecar)
	model, err := LoadProxyModel(dir, "narrow")
	if err != nil {
		t.Fatalf("LoadProxyModel: %v", err)
	}

	if _, err := model.CodecBlock("libx264", 0.5, 0.5); err != nil {
		t.Fatalf("in-vocabulary codec: %v", err)
	}
	_, err = model.CodecBlock("h264_nvenc", 0.5, 0.5)
	if err == nil {
		t.Fatal("want an error for an out-of-vocabulary codec with no 'unknown' slot")
	}
	if !containsStr(err.Error(), "no 'unknown' slot") {
		t.Errorf("error %q should explain the missing catch-all", err)
	}
}

// TestCodecBlockEmptyVocab covers a sidecar with no vocabulary at all.
func TestCodecBlockEmptyVocab(t *testing.T) {
	t.Parallel()

	sidecar := shippedSidecar()
	sidecar.EncoderVocab = nil
	dir := writeProxyFixture(t, "novocab", sidecar)
	model, err := LoadProxyModel(dir, "novocab")
	if err != nil {
		t.Fatalf("LoadProxyModel: %v", err)
	}
	if _, err := model.CodecBlock("libx264", 0.5, 0.5); err == nil {
		t.Fatal("want an error for a sidecar with no encoder_vocab")
	}
}

// TestNormaliseFeatures pins the StandardScaler the Python fast path never
// applies.
func TestNormaliseFeatures(t *testing.T) {
	t.Parallel()

	dir := writeProxyFixture(t, DefaultProxyModelID, shippedSidecar())
	model, err := LoadProxyModel(dir, DefaultProxyModelID)
	if err != nil {
		t.Fatalf("LoadProxyModel: %v", err)
	}
	sc := shippedSidecar()

	t.Run("the training mean maps to the origin", func(t *testing.T) {
		t.Parallel()
		got, err := model.NormaliseFeatures(sc.FeatureMean)
		if err != nil {
			t.Fatalf("NormaliseFeatures: %v", err)
		}
		for i, v := range got {
			if math.Abs(v) > 1e-12 {
				t.Errorf("feature %d = %v, want 0 (input was the training mean)", i, v)
			}
		}
	})

	t.Run("one std above the mean maps to 1", func(t *testing.T) {
		t.Parallel()
		in := make([]float64, len(sc.FeatureMean))
		for i := range in {
			in[i] = sc.FeatureMean[i] + sc.FeatureStd[i]
		}
		got, err := model.NormaliseFeatures(in)
		if err != nil {
			t.Fatalf("NormaliseFeatures: %v", err)
		}
		for i, v := range got {
			if math.Abs(v-1.0) > 1e-9 {
				t.Errorf("feature %d = %v, want 1", i, v)
			}
		}
	})

	t.Run("wrong-length input is rejected", func(t *testing.T) {
		t.Parallel()
		if _, err := model.NormaliseFeatures([]float64{1, 2, 3}); err == nil {
			t.Fatal("want an error for a non-canonical-6 vector")
		}
	})

	t.Run("a zero std leaves the feature unscaled instead of producing Inf", func(t *testing.T) {
		t.Parallel()
		sidecar := shippedSidecar()
		sidecar.FeatureStd = []float64{0, 0, 0, 0, 0, 0}
		sidecar.FeatureMean = []float64{0, 0, 0, 0, 0, 0}
		d := writeProxyFixture(t, "zerostd", sidecar)
		m, err := LoadProxyModel(d, "zerostd")
		if err != nil {
			t.Fatalf("LoadProxyModel: %v", err)
		}
		got, err := m.NormaliseFeatures([]float64{1, 2, 3, 4, 5, 6})
		if err != nil {
			t.Fatalf("NormaliseFeatures: %v", err)
		}
		for i, v := range got {
			if math.IsInf(v, 0) || math.IsNaN(v) {
				t.Errorf("feature %d = %v, want a finite passthrough", i, v)
			}
			if v != float64(i+1) {
				t.Errorf("feature %d = %v, want %v", i, v, i+1)
			}
		}
	})

	t.Run("a sidecar with no scaler is a passthrough", func(t *testing.T) {
		t.Parallel()
		sidecar := shippedSidecar()
		sidecar.FeatureMean = nil
		sidecar.FeatureStd = nil
		d := writeProxyFixture(t, "noscaler", sidecar)
		m, err := LoadProxyModel(d, "noscaler")
		if err != nil {
			t.Fatalf("LoadProxyModel: %v", err)
		}
		in := []float64{1, 2, 3, 4, 5, 6}
		got, err := m.NormaliseFeatures(in)
		if err != nil {
			t.Fatalf("NormaliseFeatures: %v", err)
		}
		for i := range in {
			if got[i] != in[i] {
				t.Errorf("feature %d = %v, want the unscaled %v", i, got[i], in[i])
			}
		}
	})
}

// TestORTProxyRejectsTwoPortGraph is the regression guard for the documented
// blocker: the currently shipped fr_regressor_v2 declares two named input
// ports, and flattening them into one 20-D vector would feed the graph's 6-D
// "features" port only. The proxy must refuse rather than return a
// silently-wrong score.
func TestORTProxyRejectsTwoPortGraph(t *testing.T) {
	t.Parallel()

	dir := writeProxyFixture(t, DefaultProxyModelID, shippedSidecar())
	proxy, err := NewORTProxy(dir, DefaultProxyModelID)
	if err != nil {
		t.Fatalf("NewORTProxy: %v", err)
	}

	features := shippedSidecar().FeatureMean
	_, scoreErr := proxy.Score(context.Background(), features, "libx264", 0.5, 0.25)
	if !errors.Is(scoreErr, ErrProxyPortsUnsupported) {
		t.Fatalf("want ErrProxyPortsUnsupported, got %v", scoreErr)
	}
	// The diagnostic must name both ports and the flattened width so an
	// operator can tell what would have to change.
	for _, want := range []string{"features", "codec", "20-D"} {
		if !containsStr(scoreErr.Error(), want) {
			t.Errorf("diagnostic %q missing %q", scoreErr, want)
		}
	}
}

// TestORTProxyValidatesBeforeInference verifies the input contract is checked
// before the (unavailable) inference step, so operators get the actionable
// error rather than a runner-not-found one.
func TestORTProxyValidatesBeforeInference(t *testing.T) {
	t.Parallel()

	dir := writeProxyFixture(t, DefaultProxyModelID, shippedSidecar())
	proxy, err := NewORTProxy(dir, DefaultProxyModelID)
	if err != nil {
		t.Fatalf("NewORTProxy: %v", err)
	}

	tests := []struct {
		name     string
		features []float64
		encoder  string
		wantErr  string
	}{
		{
			name:     "wrong feature count",
			features: []float64{1, 2, 3},
			encoder:  "libx264",
			wantErr:  "canonical-6",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := proxy.Score(context.Background(), tc.features, tc.encoder, 0.5, 0.5)
			if err == nil {
				t.Fatalf("want error containing %q, got nil", tc.wantErr)
			}
			if !containsStr(err.Error(), tc.wantErr) {
				t.Errorf("error %q does not contain %q", err, tc.wantErr)
			}
		})
	}
}

// TestORTProxySinglePortReachesTheRunner checks that a single-port export —
// the shape that WOULD unblock production mode — gets past the port guard and
// reaches the inference seam. Without vmafx-ort-runner on PATH the call then
// fails with ai.ErrORTRunnerNotFound, which is the expected environment error
// rather than the contract error.
func TestORTProxySinglePortReachesTheRunner(t *testing.T) {
	t.Setenv("PATH", t.TempDir())

	sidecar := shippedSidecar()
	sidecar.InputNames = []string{"input"}
	dir := writeProxyFixture(t, "single_port", sidecar)
	proxy, err := NewORTProxy(dir, "single_port")
	if err != nil {
		t.Fatalf("NewORTProxy: %v", err)
	}

	_, scoreErr := proxy.Score(context.Background(), sidecar.FeatureMean, "libx264", 0.5, 0.25)
	if scoreErr == nil {
		t.Fatal("want an inference error with no ort-runner on PATH, got nil")
	}
	if errors.Is(scoreErr, ErrProxyPortsUnsupported) {
		t.Errorf("a single-port graph must not trip the port guard: %v", scoreErr)
	}
	if !containsStr(scoreErr.Error(), "proxy inference") {
		t.Errorf("error %q should come from the inference step", scoreErr)
	}
}

// TestProxyFuncAdapter covers the test seam.
func TestProxyFuncAdapter(t *testing.T) {
	t.Parallel()

	var gotEncoder string
	var gotPreset, gotCRF float64
	p := ProxyFunc(func(_ context.Context, features []float64, encoder string, presetNorm, crfNorm float64) (float64, error) {
		gotEncoder, gotPreset, gotCRF = encoder, presetNorm, crfNorm
		return float64(len(features)) * 10, nil
	})

	got, err := p.Score(context.Background(), make([]float64, 6), "libx265", 0.5, 0.75)
	if err != nil {
		t.Fatalf("Score: %v", err)
	}
	if got != 60 {
		t.Errorf("score = %v, want 60", got)
	}
	if gotEncoder != "libx265" || gotPreset != 0.5 || gotCRF != 0.75 {
		t.Errorf("adapter dropped arguments: %q %v %v", gotEncoder, gotPreset, gotCRF)
	}
}

// TestNilORTProxyScore covers the defensive nil-model guard.
func TestNilORTProxyScore(t *testing.T) {
	t.Parallel()

	var p *ORTProxy
	if _, err := p.Score(context.Background(), make([]float64, 6), "libx264", 0, 0); err == nil {
		t.Fatal("want an error from a nil ORTProxy, got nil")
	}
}

// TestShippedModelIsTwoPort loads the real in-tree
// model/tiny/fr_regressor_v2.json and asserts the blocker is a property of the
// shipped artefact, not of the test fixtures: the model declares two named
// input ports, and its encoder vocabulary is the trainer's, not the one
// vmaftune/proxy.py hardcodes.
//
// Skipped when the model tree is not reachable (e.g. a trimmed checkout).
func TestShippedModelIsTwoPort(t *testing.T) {
	t.Parallel()

	model, err := LoadProxyModel("", DefaultProxyModelID)
	if err != nil {
		if errors.Is(err, ErrProxyModelNotFound) {
			t.Skipf("model/tiny/%s.onnx not reachable from the test working "+
				"directory; skipping the shipped-artefact check", DefaultProxyModelID)
		}
		t.Fatalf("LoadProxyModel: %v", err)
	}

	if len(model.Sidecar.InputNames) != 2 {
		t.Errorf("shipped input_names = %v; the two-port blocker documented on "+
			"ErrProxyPortsUnsupported may be stale", model.Sidecar.InputNames)
	}

	// The vocabulary drift documented on CodecBlock: the shipped model has an
	// "unknown" catch-all slot and does NOT know libaom-av1, the opposite of
	// vmaftune/proxy.py's hardcoded ENCODER_VOCAB_V2.
	if _, ok := model.vocabIndex["unknown"]; !ok {
		t.Error("shipped encoder_vocab has no 'unknown' slot; the CodecBlock " +
			"fallback note is stale")
	}
	if _, ok := model.vocabIndex["libaom-av1"]; ok {
		t.Error("shipped encoder_vocab now contains libaom-av1; the proxy.py " +
			"vocabulary-drift note on CodecBlock is stale")
	}
	if got := model.Sidecar.EncoderVocab; len(got) > 3 && got[3] != "libvvenc" {
		t.Errorf("shipped encoder_vocab[3] = %q, want libvvenc "+
			"(proxy.py puts libaom-av1 here)", got[3])
	}

	// The scaler the Python fast path never applies must be present.
	if len(model.Sidecar.FeatureMean) != len(canonical6) ||
		len(model.Sidecar.FeatureStd) != len(canonical6) {
		t.Errorf("shipped sidecar scaler has %d means / %d stds, want %d each",
			len(model.Sidecar.FeatureMean), len(model.Sidecar.FeatureStd), len(canonical6))
	}
}

// TestCanonical6Features pins the feature order and guards the accessor's copy.
func TestCanonical6Features(t *testing.T) {
	t.Parallel()

	want := []string{"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"}
	got := Canonical6Features()
	if len(got) != len(want) {
		t.Fatalf("length = %d, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("feature %d = %q, want %q", i, got[i], want[i])
		}
	}
	got[0] = "mutated"
	if Canonical6Features()[0] != "adm2" {
		t.Error("Canonical6Features must return a copy, not the package slice")
	}
}
