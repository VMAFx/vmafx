// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package sidecar is the Go port of vmaftune.sidecar — the local, on-host
// bias-correction model that rides alongside the shipped VMAF predictor
// (ADR-0394).
//
// The shipped predictor is a fixed, deterministic asset trained offline. The
// sidecar is a bias-correction term an operator trains on their own host from
// the residuals between predicted VMAF and the libvmaf score actually
// observed at encode time:
//
//	sidecar_predict(features, crf) =
//	    Predictor.PredictVMAF(features, crf) + Model.PredictCorrection(features)
//
// The shipped predictor is never mutated, so model upgrades stay
// deterministic and reproducible across hosts.
//
// # Persistence layout
//
//	${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/host-uuid
//	    Random 128-bit hex token generated on first install. Anonymous by
//	    construction: never derived from MAC, hostname, /etc/machine-id,
//	    CPUID, or anything machine-identifying. This is a load-bearing
//	    precondition for any future opt-in upload.
//
//	${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor-version>/<codec>/state.json
//	    Ridge weights, inverse Gram, the ring buffer of recent residuals, and
//	    the predictor version the sidecar was trained against.
//
// A predictor-version mismatch on load discards everything except the host
// UUID and resets to cold start. That is what makes shipped-model upgrades
// safe: the sidecar can never replay a stale correction against a refreshed
// predictor.
//
// # Algorithm
//
// Online ridge regression on the residual y = observed − predicted, with a
// closed-form Sherman-Morrison rank-1 inverse update. State is O(d²) for the
// fixed feature dimension d (14); update is O(d²) per capture, prediction
// O(d). Cold start initialises weights to zero and the inverse Gram to
// (1/λ)·I, so PredictCorrection returns exactly 0.0 and SidecarPredictor
// degenerates to Predictor until the first Update.
package sidecar

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/tune/predictor"
	"github.com/VMAFx/vmafx/pkg/tune/pyjson"
)

// SchemaVersion is the on-disk state schema. Bump on every
// backwards-incompatible change to the JSON layout.
const SchemaVersion = 1

// FeatureDim is the fixed feature-vector length the sidecar consumes. It must
// stay in lockstep with FeatureVector; changing the dimensionality forces a
// SchemaVersion bump because older saved state cannot be loaded.
const FeatureDim = 14

// DefaultPredictorVersion is the predictor-version namespace tag. The shipped
// predictor does not yet expose its own version string; when it does, switch
// to that and keep this as the final fallback.
const DefaultPredictorVersion = "predictor_v1"

// DefaultCacheDir returns the default sidecar cache root, honouring
// XDG_CACHE_HOME per the freedesktop base-directory spec and falling back to
// ~/.cache on hosts that do not set it.
func DefaultCacheDir() string {
	if xdg := os.Getenv("XDG_CACHE_HOME"); xdg != "" {
		return filepath.Join(xdg, "vmaf-tune", "sidecar")
	}
	home, err := os.UserHomeDir()
	if err != nil {
		// No home directory (e.g. a container running as an unmapped uid):
		// fall back to a relative path rather than writing to /.
		return filepath.Join(".cache", "vmaf-tune", "sidecar")
	}
	return filepath.Join(home, ".cache", "vmaf-tune", "sidecar")
}

// Config is the static configuration for a local sidecar.
type Config struct {
	// CacheDir roots the state and host-UUID persistence.
	CacheDir string
	// HostUUID pins the anonymous per-install token. Empty lets the loader
	// generate or read it lazily — the standard path; tests pin it for
	// determinism.
	HostUUID string
	// LambdaL2 is the ridge regularisation strength. Higher values pull the
	// correction toward zero, defending against a single outlier capture.
	LambdaL2 float64
	// MaxHistoryRows bounds the residual ring buffer the drift-detection hook
	// reads. It does not affect the ridge fit itself.
	MaxHistoryRows int
	// TrainingCadence is reserved: the sidecar currently updates per capture.
	TrainingCadence string
	// PredictorVersion is persisted alongside the state; a mismatch on load
	// discards the old state.
	PredictorVersion string
}

// DefaultConfig returns the configuration the CLI uses when the operator
// overrides nothing.
func DefaultConfig() Config {
	return Config{
		CacheDir:         DefaultCacheDir(),
		LambdaL2:         1.0,
		MaxHistoryRows:   500,
		TrainingCadence:  "per-capture",
		PredictorVersion: DefaultPredictorVersion,
	}
}

// normalise fills in the zero-value fields a caller left unset.
func (c Config) normalise() Config {
	def := DefaultConfig()
	if c.CacheDir == "" {
		c.CacheDir = def.CacheDir
	}
	if c.LambdaL2 == 0 {
		c.LambdaL2 = def.LambdaL2
	}
	if c.MaxHistoryRows == 0 {
		c.MaxHistoryRows = def.MaxHistoryRows
	}
	if c.TrainingCadence == "" {
		c.TrainingCadence = def.TrainingCadence
	}
	if c.PredictorVersion == "" {
		c.PredictorVersion = def.PredictorVersion
	}
	return c
}

// FeatureVector materialises the fixed-dim vector the sidecar consumes.
//
// The order is load-bearing: it pins the column index of every ridge weight.
// Changing it requires bumping SchemaVersion, because an older saved state
// would otherwise align mismatched columns to the wrong feature. The leading
// 1.0 is the bias term so the fit can model a constant predicted-vs-observed
// offset.
func FeatureVector(f predictor.ShotFeatures, crf int) ([]float64, error) {
	vec := []float64{
		1.0, // bias
		float64(crf),
		f.ProbeBitrateKbps,
		f.ProbeIFrameAvgBytes,
		f.ProbePFrameAvgBytes,
		f.ProbeBFrameAvgBytes,
		f.SaliencyMean,
		f.SaliencyVar,
		f.FrameDiffMean,
		f.YAvg,
		f.YVar,
		float64(f.ShotLengthFrames),
		f.FPS,
		float64(f.Width),
	}
	if len(vec) != FeatureDim {
		return nil, fmt.Errorf(
			"sidecar feature vector length %d != FeatureDim=%d; "+
				"bump SchemaVersion when changing the layout", len(vec), FeatureDim)
	}
	return vec, nil
}

// HistoryRow is one entry of the residual ring buffer.
type HistoryRow struct {
	Residual      float64 `json:"residual"`
	PredictedVMAF float64 `json:"predicted_vmaf"`
	ObservedVMAF  float64 `json:"observed_vmaf"`
}

// Model is the online-ridge bias-correction state.
//
// It maintains a weight vector w and the inverse Gram matrix AInv such that
// w = AInv · (Xᵀy) over all observed (features, residual) pairs. The
// Sherman-Morrison rank-1 update keeps both in sync per capture in O(d²) time
// with no numerical solver.
type Model struct {
	Config   Config
	Weights  []float64
	AInv     [][]float64
	History  []HistoryRow
	NUpdates int
}

// NewModel returns a cold-start model: zero weights and AInv = (1/λ)·I. With
// zero weights PredictCorrection returns exactly 0.0.
func NewModel(cfg Config) *Model {
	cfg = cfg.normalise()
	return &Model{
		Config:  cfg,
		Weights: make([]float64, FeatureDim),
		AInv:    eyeScaled(FeatureDim, 1.0/cfg.LambdaL2),
	}
}

func eyeScaled(d int, scale float64) [][]float64 {
	out := make([][]float64, d)
	for i := range out {
		row := make([]float64, d)
		row[i] = scale
		out[i] = row
	}
	return out
}

// Update folds one (features, residual) pair into the ridge fit.
//
// The residual is y = observed − predicted; the sidecar learns to predict
// that residual from the features. A non-finite or non-positive
// Sherman-Morrison denominator (only reachable with NaN/Inf features) skips
// the update rather than corrupting the state.
func (m *Model) Update(f predictor.ShotFeatures, observedVMAF, predictedVMAF float64, crf int) error {
	x, err := FeatureVector(f, crf)
	if err != nil {
		return err
	}
	residual := observedVMAF - predictedVMAF

	// Sherman-Morrison: AInv' = AInv − (AInv x xᵀ AInv) / (1 + xᵀ AInv x)
	aInvX := matvec(m.AInv, x)
	denom := 1.0 + dot(x, aInvX)
	if math.IsNaN(denom) || math.IsInf(denom, 0) || denom <= 0.0 {
		return nil
	}
	outerAXPY(m.AInv, aInvX, aInvX, -1.0/denom)

	// Recursive-least-squares weight update on the centred residual.
	// AInv x is re-derived against the updated AInv for numerical stability.
	predictionError := residual - dot(x, m.Weights)
	coeff := predictionError / denom
	aInvXNew := matvec(m.AInv, x)
	for i := 0; i < FeatureDim; i++ {
		m.Weights[i] += coeff * aInvXNew[i]
	}

	m.History = append(m.History, HistoryRow{
		Residual:      residual,
		PredictedVMAF: predictedVMAF,
		ObservedVMAF:  observedVMAF,
	})
	if len(m.History) > m.Config.MaxHistoryRows {
		m.History = m.History[len(m.History)-m.Config.MaxHistoryRows:]
	}
	m.NUpdates++
	return nil
}

// PredictCorrection returns the additive correction in VMAF units. Cold start
// (no captures) returns 0.0.
func (m *Model) PredictCorrection(f predictor.ShotFeatures, crf int) float64 {
	x, err := FeatureVector(f, crf)
	if err != nil {
		return 0.0
	}
	return dot(m.Weights, x)
}

// RecentResidualRMS is the drift-detection signal: the RMS of the residuals
// still in the ring buffer, or 0.0 at cold start.
func (m *Model) RecentResidualRMS() float64 {
	if len(m.History) == 0 {
		return 0.0
	}
	sum := 0.0
	for _, row := range m.History {
		sum += row.Residual * row.Residual
	}
	return math.Sqrt(sum / float64(len(m.History)))
}

// ---------------------------------------------------------------------------
// Linear-algebra kernels. Deliberately dependency-free: the sidecar sits on
// vmaf-tune's harness hot path and must not pull a BLAS binding in.
// ---------------------------------------------------------------------------

func matvec(m [][]float64, v []float64) []float64 {
	out := make([]float64, len(m))
	for i, row := range m {
		s := 0.0
		for j, vj := range v {
			s += row[j] * vj
		}
		out[i] = s
	}
	return out
}

func dot(a, b []float64) float64 {
	s := 0.0
	for i, ai := range a {
		s += ai * b[i]
	}
	return s
}

// outerAXPY performs m += alpha * outer(u, v) in place.
func outerAXPY(m [][]float64, u, v []float64, alpha float64) {
	for i, ui := range u {
		scale := alpha * ui
		if scale == 0.0 {
			continue
		}
		row := m[i]
		for j, vj := range v {
			row[j] += scale * vj
		}
	}
}

// ---------------------------------------------------------------------------
// Persistence.
// ---------------------------------------------------------------------------

// ToMap serialises the model to the JSON-compatible shape the Python
// SidecarModel.to_dict produces.
func (m *Model) ToMap() map[string]any {
	weights := make([]any, len(m.Weights))
	for i, w := range m.Weights {
		weights[i] = w
	}
	aInv := make([]any, len(m.AInv))
	for i, row := range m.AInv {
		cols := make([]any, len(row))
		for j, v := range row {
			cols[j] = v
		}
		aInv[i] = cols
	}
	history := make([]any, len(m.History))
	for i, row := range m.History {
		history[i] = map[string]any{
			"residual":       row.Residual,
			"predicted_vmaf": row.PredictedVMAF,
			"observed_vmaf":  row.ObservedVMAF,
		}
	}
	return map[string]any{
		"schema_version":    SchemaVersion,
		"predictor_version": m.Config.PredictorVersion,
		"feature_dim":       FeatureDim,
		"lambda_l2":         m.Config.LambdaL2,
		"weights":           weights,
		"a_inv":             aInv,
		"history":           history,
		"n_updates":         m.NUpdates,
	}
}

// stateDoc is the on-disk state, used for loading.
type stateDoc struct {
	SchemaVersion    int          `json:"schema_version"`
	PredictorVersion string       `json:"predictor_version"`
	FeatureDim       int          `json:"feature_dim"`
	LambdaL2         float64      `json:"lambda_l2"`
	Weights          []float64    `json:"weights"`
	AInv             [][]float64  `json:"a_inv"`
	History          []HistoryRow `json:"history"`
	NUpdates         int          `json:"n_updates"`
}

// ModelFromMap reconstructs a Model from a decoded state document. It returns
// an error when the schema version, feature dimensionality, predictor
// version, or matrix shape does not match — Load catches that and falls back
// to cold start.
func ModelFromMap(doc stateDoc, cfg Config) (*Model, error) {
	cfg = cfg.normalise()
	if doc.SchemaVersion != SchemaVersion {
		return nil, fmt.Errorf("sidecar schema version %d != %d", doc.SchemaVersion, SchemaVersion)
	}
	if doc.FeatureDim != FeatureDim {
		return nil, fmt.Errorf("sidecar feature_dim %d != %d", doc.FeatureDim, FeatureDim)
	}
	if doc.PredictorVersion != cfg.PredictorVersion {
		return nil, fmt.Errorf("sidecar predictor_version %q != %q",
			doc.PredictorVersion, cfg.PredictorVersion)
	}
	if len(doc.Weights) != FeatureDim || len(doc.AInv) != FeatureDim {
		return nil, errors.New("sidecar state has wrong shape")
	}
	return &Model{
		Config:   cfg,
		Weights:  doc.Weights,
		AInv:     doc.AInv,
		History:  doc.History,
		NUpdates: doc.NUpdates,
	}, nil
}

// Save writes the state to path as JSON, creating parents. Atomic-ish: write
// to a .tmp sibling, then rename.
func (m *Model) Save(path string) error {
	payload, err := pyjson.MarshalStrict(m.ToMap(), 2)
	if err != nil {
		return err
	}
	return writeAtomic(path, payload+"\n")
}

func writeAtomic(path, content string) error {
	// G301: 0o750 keeps the cache directory owner-and-group readable only.
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("create sidecar dir: %w", err)
	}
	tmp := path + ".tmp"
	// G306: 0o600 — the sidecar state carries host-local residuals.
	if err := os.WriteFile(tmp, []byte(content), 0o600); err != nil {
		return fmt.Errorf("write sidecar state: %w", err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return fmt.Errorf("commit sidecar state: %w", err)
	}
	return nil
}

// Load reads state from path, or returns a cold-start model.
//
// A predictor-version or schema-version mismatch returns cold start rather
// than an error, and so does corrupt JSON — the corrupt file is left in place
// so the operator can inspect it.
func Load(path string, cfg Config) *Model {
	cfg = cfg.normalise()
	info, err := os.Stat(path)
	if err != nil || info.IsDir() {
		return NewModel(cfg)
	}
	data, err := os.ReadFile(path) // #nosec G304 -- path is derived from the operator's --cache-dir
	if err != nil {
		return NewModel(cfg)
	}
	var doc stateDoc
	if err := json.Unmarshal(data, &doc); err != nil {
		return NewModel(cfg)
	}
	model, err := ModelFromMap(doc, cfg)
	if err != nil {
		return NewModel(cfg)
	}
	return model
}

// ---------------------------------------------------------------------------
// Host UUID.
// ---------------------------------------------------------------------------

// HostUUIDPath is where the anonymous random host UUID lives. It sits at the
// cache-dir root, above the per-predictor-version subdirectories, so it
// survives predictor upgrades.
func HostUUIDPath(cacheDir string) string {
	return filepath.Join(cacheDir, "host-uuid")
}

// GetOrCreateHostUUID returns the anonymous host UUID, creating it on first
// call.
//
// The UUID is 32 hex characters (128 bits) from crypto/rand. It is never
// derived from MAC, hostname, /etc/machine-id, CPUID, or any other
// machine-identifying signal — a load-bearing precondition for any future
// opt-in upload.
func GetOrCreateHostUUID(cacheDir string) (string, error) {
	if err := os.MkdirAll(cacheDir, 0o750); err != nil {
		return "", fmt.Errorf("create sidecar cache dir: %w", err)
	}
	path := HostUUIDPath(cacheDir)
	if data, err := os.ReadFile(path); err == nil { // #nosec G304 -- operator-configured cache dir
		if uuid := strings.TrimSpace(string(data)); uuid != "" {
			return uuid, nil
		}
	}
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate host uuid: %w", err)
	}
	uuid := hex.EncodeToString(raw)
	if err := writeAtomic(path, uuid+"\n"); err != nil {
		return "", err
	}
	return uuid, nil
}

// ---------------------------------------------------------------------------
// Composed predictor.
// ---------------------------------------------------------------------------

// Predictor composes a base predictor with a sidecar Model. PredictVMAF
// delegates to the base and adds the correction; callers feed captured
// (features, observed) pairs back through RecordCapture, which also persists
// the state.
type Predictor struct {
	Base      *predictor.Predictor
	Model     *Model
	Codec     string
	StatePath string
	HostUUID  string
}

// StatePathFor returns the canonical per-codec state path.
func StatePathFor(cfg Config, codecName string) string {
	cfg = cfg.normalise()
	return filepath.Join(cfg.CacheDir, cfg.PredictorVersion, codecName, "state.json")
}

// ForCodec wires a sidecar for base against codecName. Existing state is
// loaded from the canonical cache path when it exists *and* its recorded
// predictor version matches; otherwise the sidecar starts cold.
func ForCodec(base *predictor.Predictor, codecName string, cfg Config) (*Predictor, error) {
	cfg = cfg.normalise()
	hostUUID := cfg.HostUUID
	if hostUUID == "" {
		generated, err := GetOrCreateHostUUID(cfg.CacheDir)
		if err != nil {
			return nil, err
		}
		hostUUID = generated
	}
	statePath := StatePathFor(cfg, codecName)
	return &Predictor{
		Base:      base,
		Model:     Load(statePath, cfg),
		Codec:     codecName,
		StatePath: statePath,
		HostUUID:  hostUUID,
	}, nil
}

// PredictVMAF returns the base prediction with the sidecar correction folded
// in, clamped to [0, 100] so a runaway correction cannot push the score
// outside the VMAF range.
func (p *Predictor) PredictVMAF(f predictor.ShotFeatures, crf int, codecName string) float64 {
	c := codecName
	if c == "" {
		c = p.Codec
	}
	base := p.Base.PredictVMAF(f, crf, c)
	correction := p.Model.PredictCorrection(f, crf)
	return math.Min(math.Max(base+correction, 0.0), 100.0)
}

// RecordCapture folds one observed VMAF into the sidecar fit.
//
// The bare-predictor prediction (sidecar excluded) is computed against the
// same features / CRF / codec, then the residual observed − predicted is
// folded into the ridge state. Persists unless persist is false.
func (p *Predictor) RecordCapture(
	f predictor.ShotFeatures,
	crf int,
	observedVMAF float64,
	codecName string,
	persist bool,
) error {
	c := codecName
	if c == "" {
		c = p.Codec
	}
	predicted := p.Base.PredictVMAF(f, crf, c)
	if err := p.Model.Update(f, observedVMAF, predicted, crf); err != nil {
		return err
	}
	if persist {
		return p.Save()
	}
	return nil
}

// Save persists the sidecar state to the configured cache path.
func (p *Predictor) Save() error {
	return p.Model.Save(p.StatePath)
}
