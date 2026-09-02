// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package fast

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/ai"
)

// DefaultProxyModelID is the registry id of the fast-path proxy model, matching
// model/tiny/registry.json and vmaftune.proxy.DEFAULT_PROXY_MODEL_ID.
const DefaultProxyModelID = "fr_regressor_v2"

// canonical6 is the libvmaf feature order fr_regressor_v2 was trained on
// (vmaftune.CANONICAL6_FEATURES / the sidecar's feature_order).
var canonical6 = []string{"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"}

// Canonical6Features returns the canonical-6 libvmaf feature names in the
// order fr_regressor_v2 expects them.
func Canonical6Features() []string {
	out := make([]string, len(canonical6))
	copy(out, canonical6)
	return out
}

// ErrProxyPortsUnsupported reports that the resolved proxy model declares more
// than one ONNX input port, which the Go ONNX seam cannot drive.
//
// # Why this is a hard blocker, not a stub
//
// fr_regressor_v2 is exported from FRRegressor.forward(features, codec_onehot)
// with two named input ports — "features" (shape [N, 6]) and "codec"
// (shape [N, 14]) — as recorded in model/tiny/fr_regressor_v2.json's
// "input_names": ["features", "codec"].
//
// The only ONNX inference seam in the Go tree is pkg/ai.Registry.Infer, which
// serialises ONE flat []float64 to the vmafx-ort-runner subprocess
// (`--inputs '[...]'`). There is no wire format for a second named port: the
// runner (cmd/vmafx-ort-runner, ADR-1134) binds that one array to the graph's
// single input, and libvmaf's session rejects a two-input graph with an arity
// error. pkg/ai.Registry.InferDirect — the CGO path via a real ONNX Runtime
// binding — is an explicit Stage-2 stub returning ErrDirectInferNotImplemented.
//
// Concatenating the two ports into a single 20-D vector is NOT a workaround:
// vmaftune/proxy.py documents that exact mistake ("the first linear layer of
// the exported graph reads the 6-D 'features' port only, so the 14 codec dims
// were silently interpreted as batch padding and the 'codec' port received
// nothing, breaking fast-path production mode"). Producing a silently-wrong
// number here would be worse than failing.
//
// Unblocking needs ONE of:
//   - a vmafx-ort-runner protocol that accepts named input tensors, plus a
//     matching pkg/ai.Registry.InferNamed (the runner is in-tree, so this is
//     a protocol extension rather than an external dependency); or
//   - promoting pkg/ai.Registry.InferDirect onto a CGO ONNX Runtime binding
//     (e.g. github.com/yalue/onnxruntime_go), which pkg/ai defers to Stage 2
//     precisely because it couples the build to libonnxruntime; or
//   - a single-port re-export of fr_regressor_v2 that concatenates the two
//     inputs *inside* the graph, shipped alongside the current model.
var ErrProxyPortsUnsupported = errors.New(
	"fast: proxy model declares multiple ONNX input ports; the Go ORT seam " +
		"(pkg/ai.Registry.Infer -> vmafx-ort-runner) accepts a single flat input vector")

// ErrProxyModelNotFound reports that the proxy model / sidecar could not be
// located on disk.
var ErrProxyModelNotFound = errors.New("fast: proxy model not found")

// ProxySidecar is the subset of model/tiny/<id>.json the proxy needs.
//
// Reading the vocabulary and the scaler from the sidecar rather than hardcoding
// them is deliberate: the Python vmaftune/proxy.py hardcodes its own
// ENCODER_VOCAB_V2 tuple, and that tuple has drifted out of sync with the
// shipped model (see ProxyModel.CodecBlock for the details), which silently
// mis-slots the one-hot for most codecs. A sidecar-driven block cannot drift.
type ProxySidecar struct {
	ID           string    `json:"id"`
	ONNX         string    `json:"onnx"`
	InputNames   []string  `json:"input_names"`
	OutputNames  []string  `json:"output_names"`
	EncoderVocab []string  `json:"encoder_vocab"`
	FeatureOrder []string  `json:"feature_order"`
	FeatureMean  []float64 `json:"feature_mean"`
	FeatureStd   []float64 `json:"feature_std"`
}

// ProxyModel is a resolved fast-path proxy: the ONNX file, its sidecar, and
// the derived encoder-vocabulary index.
type ProxyModel struct {
	// ModelID is the registry id (e.g. "fr_regressor_v2").
	ModelID string
	// ONNXPath is the resolved .onnx file path.
	ONNXPath string
	// SidecarPath is the resolved .json sidecar path.
	SidecarPath string
	// Sidecar is the parsed sidecar.
	Sidecar ProxySidecar

	vocabIndex map[string]int
}

// LoadProxyModel resolves and parses the proxy model sidecar.
//
// modelDir may be:
//   - a directory holding <modelID>.onnx and <modelID>.json (e.g. model/tiny),
//   - empty, in which case VMAFX_MODEL_DIR/tiny is tried, then a walk up from
//     the working directory looking for model/tiny/<modelID>.onnx (the
//     in-tree layout vmaftune.proxy._resolve_model_path walks).
func LoadProxyModel(modelDir, modelID string) (*ProxyModel, error) {
	if modelID == "" {
		modelID = DefaultProxyModelID
	}
	if err := validateModelID(modelID); err != nil {
		return nil, err
	}
	dir, err := resolveProxyDir(modelDir, modelID)
	if err != nil {
		return nil, err
	}

	onnxPath := filepath.Join(dir, modelID+".onnx")
	sidecarPath := filepath.Join(dir, modelID+".json")

	// #nosec G304 -- sidecarPath is built from the resolved registry
	// directory plus the caller-supplied model id, the same trust level as
	// the model file itself.
	raw, readErr := os.ReadFile(sidecarPath)
	if readErr != nil {
		return nil, fmt.Errorf("%w: read sidecar %q: %w", ErrProxyModelNotFound, sidecarPath, readErr)
	}
	var sidecar ProxySidecar
	if err := json.Unmarshal(raw, &sidecar); err != nil {
		return nil, fmt.Errorf("fast: parse proxy sidecar %q: %w", sidecarPath, err)
	}

	m := &ProxyModel{
		ModelID:     modelID,
		ONNXPath:    onnxPath,
		SidecarPath: sidecarPath,
		Sidecar:     sidecar,
		vocabIndex:  make(map[string]int, len(sidecar.EncoderVocab)),
	}
	for i, name := range sidecar.EncoderVocab {
		m.vocabIndex[name] = i
	}
	return m, nil
}

// validateModelID rejects anything that is not a bare registry identifier.
//
// A model id is a key in model/tiny/registry.json — letters, digits, dot,
// dash and underscore — never a path. Enforcing that here keeps a
// caller-supplied id from escaping the resolved model directory when it is
// joined into a filesystem path (CWE-22), and gives a clearer error than a
// mysterious not-found.
func validateModelID(modelID string) error {
	if modelID == "" {
		return errors.New("fast: proxy model id must not be empty")
	}
	if modelID != filepath.Base(modelID) || modelID == "." || modelID == ".." {
		return fmt.Errorf("fast: proxy model id %q must be a bare registry id, not a path", modelID)
	}
	for _, r := range modelID {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
		case r == '_', r == '-', r == '.':
		default:
			return fmt.Errorf(
				"fast: proxy model id %q contains %q; allowed characters are "+
					"letters, digits, '_', '-' and '.'", modelID, r)
		}
	}
	return nil
}

// resolveProxyDir finds the directory holding <modelID>.onnx.
//
// modelID must already have passed validateModelID: it is joined into a
// filesystem path, so a bare identifier is what keeps the join inside the
// resolved directory.
//
// Note: this does not reuse pkg/libvmaf.RepoRoot even though that helper
// exists. pkg/libvmaf carries a cgo-gated direct-scoring path, and pulling it
// into vmafx-tune-go would make the tuning CLI's build depend on libvmaf
// headers being installed. The walk below is the same three-line search the
// Python proxy performs.
func resolveProxyDir(modelDir, modelID string) (string, error) {
	if modelDir != "" {
		if _, err := os.Stat(filepath.Join(modelDir, modelID+".onnx")); err != nil {
			return "", fmt.Errorf("%w: %s/%s.onnx", ErrProxyModelNotFound, modelDir, modelID)
		}
		return modelDir, nil
	}

	var candidates []string
	if env := os.Getenv("VMAFX_MODEL_DIR"); env != "" {
		candidates = append(candidates, filepath.Join(env, "tiny"), env)
	}
	candidates = append(candidates,
		filepath.Join(ai.DefaultModelDir, "tiny"),
		ai.DefaultModelDir,
	)

	// Walk up from the working directory looking for model/tiny/<id>.onnx —
	// the in-tree layout ADR-0291 pins.
	if wd, err := os.Getwd(); err == nil {
		for dir := wd; ; {
			candidates = append(candidates, filepath.Join(dir, "model", "tiny"))
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}

	for _, c := range candidates {
		// #nosec G703 -- modelID is validated by validateModelID before this
		// function is reached: it must equal filepath.Base(modelID) and
		// contain only [A-Za-z0-9_.-], so it cannot traverse out of `c`.
		// gosec's taint analysis does not recognise that check as a
		// sanitiser. `c` itself is either an operator-set VMAFX_MODEL_DIR, a
		// build-time constant, or an ancestor of the working directory.
		if _, err := os.Stat(filepath.Join(c, modelID+".onnx")); err == nil {
			return c, nil
		}
	}
	return "", fmt.Errorf(
		"%w: could not locate %s.onnx; set VMAFX_MODEL_DIR or run from the repo "+
			"(searched %s)", ErrProxyModelNotFound, modelID, strings.Join(candidates, ", "))
}

// CodecBlock builds the codec block the model concatenates onto the feature
// vector: a one-hot over the sidecar's encoder_vocab followed by preset_norm
// and crf_norm.
//
// # Divergence from vmaftune/proxy.py
//
// proxy.py hardcodes
//
//	ENCODER_VOCAB_V2 = (libx264, libx265, libsvtav1, libaom-av1, libvpx-vp9,
//	                    libvvenc, h264_nvenc, hevc_nvenc, av1_nvenc,
//	                    h264_qsv, hevc_qsv, av1_qsv)
//
// but the trainer that produced the shipped checkpoint
// (ai/scripts/train_fr_regressor_v2.py, ENCODER_VOCAB) and the shipped
// sidecar (model/tiny/fr_regressor_v2.json, "encoder_vocab") both use
//
//	(libx264, libx265, libsvtav1, libvvenc, libvpx-vp9, h264_nvenc,
//	 hevc_nvenc, av1_nvenc, h264_qsv, hevc_qsv, av1_qsv, unknown)
//
// The two agree only on the first three slots. Everything from index 3 on is
// mis-slotted by proxy.py, and "libaom-av1" — which the model never saw — is
// accepted while "unknown", the model's own catch-all slot, is rejected.
// This port reads the vocabulary from the sidecar, so it is correct by
// construction and cannot drift from whatever checkpoint is installed.
//
// An encoder outside the vocabulary maps to the "unknown" slot when the model
// has one, and is otherwise a hard error — a silent zero-vector would ship
// out-of-distribution predictions unnoticed.
func (m *ProxyModel) CodecBlock(encoder string, presetNorm, crfNorm float64) ([]float64, error) {
	vocab := m.Sidecar.EncoderVocab
	if len(vocab) == 0 {
		return nil, fmt.Errorf("fast: proxy sidecar %q declares no encoder_vocab", m.SidecarPath)
	}
	idx, ok := m.vocabIndex[encoder]
	if !ok {
		fallback, hasUnknown := m.vocabIndex["unknown"]
		if !hasUnknown {
			return nil, fmt.Errorf(
				"fast: encoder %q not in the proxy model's encoder_vocab and the model "+
					"has no 'unknown' slot (vocab: %s)", encoder, strings.Join(vocab, ", "))
		}
		idx = fallback
	}
	block := make([]float64, len(vocab)+2)
	block[idx] = 1.0
	block[len(vocab)] = presetNorm
	block[len(vocab)+1] = crfNorm
	return block, nil
}

// NormaliseFeatures applies the sidecar's StandardScaler to a canonical-6
// feature vector: (x - feature_mean) / feature_std, elementwise.
//
// # Divergence from the Python fast path
//
// vmaftune/proxy.py's run_proxy docstring says "Caller is responsible for
// StandardScaler normalisation; this helper does NOT re-normalise (the trained
// scaler lives in the ai/ training tree)" — but no caller on the fast path
// does it. cli._build_fast_sample_extractor returns raw libvmaf pooled means,
// fast._build_prod_predictor forwards them unchanged to _proxy_score, and
// run_proxy feeds them straight into the graph. The shipped sidecar carries
// feature_mean / feature_std precisely so this step can happen; the Go port
// performs it.
//
// A zero or missing std leaves the feature unscaled rather than producing
// ±Inf.
func (m *ProxyModel) NormaliseFeatures(features []float64) ([]float64, error) {
	if len(features) != len(canonical6) {
		return nil, fmt.Errorf("fast: features must be the canonical-6 vector; got length %d",
			len(features))
	}
	mean, std := m.Sidecar.FeatureMean, m.Sidecar.FeatureStd
	out := make([]float64, len(features))
	for i, v := range features {
		out[i] = v
		if i < len(mean) {
			out[i] -= mean[i]
		}
		if i < len(std) && std[i] != 0 && !math.IsNaN(std[i]) {
			out[i] /= std[i]
		}
	}
	return out, nil
}

// Proxy scores a canonical-6 feature vector into a predicted VMAF value.
// Production wires ORTProxy; tests inject a deterministic stub.
type Proxy interface {
	Score(ctx context.Context, features []float64, encoder string, presetNorm, crfNorm float64) (float64, error)
}

// ProxyFunc adapts a plain function to the Proxy interface.
type ProxyFunc func(ctx context.Context, features []float64, encoder string, presetNorm, crfNorm float64) (float64, error)

// Score implements Proxy.
func (f ProxyFunc) Score(ctx context.Context, features []float64, encoder string, presetNorm, crfNorm float64) (float64, error) {
	return f(ctx, features, encoder, presetNorm, crfNorm)
}

// ORTProxy runs fr_regressor_v2 through the Go ONNX seam
// (pkg/ai.Registry.Infer -> vmafx-ort-runner).
//
// See ErrProxyPortsUnsupported: for the currently shipped two-port
// fr_regressor_v2 export, Score always fails with that error rather than
// feeding the graph a wrong tensor layout. Score succeeds only against a
// single-port export.
type ORTProxy struct {
	// Model is the resolved proxy model.
	Model *ProxyModel
	// Registry is the ONNX inference seam. nil selects a registry rooted at
	// the model's own directory.
	Registry *ai.Registry
}

// NewORTProxy resolves the proxy model and returns an ORTProxy over it.
func NewORTProxy(modelDir, modelID string) (*ORTProxy, error) {
	model, err := LoadProxyModel(modelDir, modelID)
	if err != nil {
		return nil, err
	}
	return &ORTProxy{
		Model:    model,
		Registry: ai.NewRegistry(filepath.Dir(model.ONNXPath)),
	}, nil
}

// Score implements Proxy. It normalises the canonical-6 vector with the
// sidecar scaler, builds the codec block, and runs the graph.
func (p *ORTProxy) Score(
	ctx context.Context,
	features []float64,
	encoder string,
	presetNorm, crfNorm float64,
) (float64, error) {
	if p == nil || p.Model == nil {
		return 0, errors.New("fast: ORTProxy has no resolved model")
	}

	scaled, err := p.Model.NormaliseFeatures(features)
	if err != nil {
		return 0, err
	}
	block, err := p.Model.CodecBlock(encoder, presetNorm, crfNorm)
	if err != nil {
		return 0, err
	}

	// Hard stop before inference: a two-port graph cannot be driven through
	// the single-flat-vector runner protocol, and flattening the ports would
	// silently mis-feed the first dense layer.
	if len(p.Model.Sidecar.InputNames) > 1 {
		return 0, fmt.Errorf(
			"%w: model %q declares input ports %v (sidecar %s). "+
				"Flattening them into one %d-D vector would feed the graph's 6-D "+
				"'features' port only and leave 'codec' empty — see the note on "+
				"ErrProxyPortsUnsupported for the three ways to unblock this",
			ErrProxyPortsUnsupported, p.Model.ModelID, p.Model.Sidecar.InputNames,
			p.Model.SidecarPath, len(scaled)+len(block))
	}

	registry := p.Registry
	if registry == nil {
		registry = ai.NewRegistry(filepath.Dir(p.Model.ONNXPath))
	}
	inputs := make([]float64, 0, len(scaled)+len(block))
	inputs = append(inputs, scaled...)
	inputs = append(inputs, block...)

	outputs, inferErr := registry.Infer(ctx, p.Model.ModelID, inputs)
	if inferErr != nil {
		return 0, fmt.Errorf("fast: proxy inference: %w", inferErr)
	}
	if len(outputs) == 0 {
		return 0, errors.New("fast: proxy inference returned no outputs")
	}
	return outputs[0], nil
}

// Compile-time check.
var _ Proxy = (*ORTProxy)(nil)
