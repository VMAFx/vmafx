// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/sidecar/sidecar.go — Go port of vmaftune.sidecar (ADR-0325 / ADR-0394).
//
// The shipped predictor is a fixed, deterministic asset. The sidecar is a
// bias-correction term an operator trains on their own host from the residuals
// between predicted VMAF and the libvmaf score actually observed at encode
// time:
//
//	sidecar_predict(features, crf, codec) =
//	    Predictor.PredictVMAF(features, crf, codec) + Model.PredictCorrection(features, crf)
//
// The shipped predictor is never mutated — model upgrades stay deterministic
// and reproducible across hosts.
//
// # Persistence layout
//
//   - ${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/host-uuid — a random
//     128-bit hex token generated on first install. Anonymous by construction;
//     never derived from MAC, hostname, /etc/machine-id, or CPUID.
//   - ${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor-version>/<codec>/state.json
//     — ridge weights, inverse Gram, ring buffer of recent residuals, and the
//     predictor_version the sidecar was trained against.
//
// A predictor-version mismatch on load discards everything except the host
// UUID and resets to cold start (zero correction). That is what makes shipped
// model upgrades safe: the sidecar can never replay a stale correction against
// a refreshed predictor.
//
// # Algorithm
//
// Online ridge regression on the residual y = observed − predicted with a
// closed-form Sherman-Morrison rank-1 inverse update. State size is O(d²)
// where d is FeatureDim; update cost is O(d²) per capture and prediction cost
// is O(d). Cold start initialises weights to zero and A_inv to (1/lambda)·I,
// so PredictCorrection returns exactly 0.0 until the first Update.
package sidecar

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"

	"github.com/VMAFx/vmafx/pkg/predictor"
)

// SchemaVersion is the on-disk sidecar state schema. Bump on every
// backwards-incompatible change to the JSON layout.
const SchemaVersion = 1

// FeatureDim is the fixed-dim feature-vector length the sidecar consumes.
// It must stay in lockstep with FeatureVector; changing the dimensionality
// forces a SchemaVersion bump (older saved state cannot be loaded).
const FeatureDim = 14

// DefaultPredictorVersion is the predictor-version tag the sidecar persists
// alongside its state.
const DefaultPredictorVersion = "predictor_v1"

// DefaultCacheDir returns the default sidecar cache directory, honouring
// XDG_CACHE_HOME per the freedesktop base-directory spec and falling back to
// ~/.cache on hosts that do not set it.
func DefaultCacheDir() string {
	if xdg := os.Getenv("XDG_CACHE_HOME"); xdg != "" {
		return filepath.Join(xdg, "vmaf-tune", "sidecar")
	}
	home, err := os.UserHomeDir()
	if err != nil {
		// Mirror pathlib.Path.home()'s behaviour of falling back to the
		// process CWD-relative path rather than failing the command.
		home = ""
	}
	return filepath.Join(home, ".cache", "vmaf-tune", "sidecar")
}

// Config is the static configuration for a local sidecar.
type Config struct {
	// CacheDir is the root under which the sidecar persists its state and
	// the host UUID. Empty means DefaultCacheDir().
	CacheDir string
	// HostUUID pins the anonymous per-install token. Empty lets the model
	// generate / load it lazily — the standard path; tests pin it.
	HostUUID string
	// LambdaL2 is the L2 regularisation strength. Higher values pull the
	// correction toward zero, defending against single-outlier captures.
	LambdaL2 float64
	// MaxHistoryRows bounds the ring buffer of captured residuals the
	// drift-detection hook reads. It does not affect the ridge fit itself.
	MaxHistoryRows int
	// TrainingCadence is reserved for future use.
	TrainingCadence string
	// PredictorVersion is persisted alongside the state; a mismatch on load
	// discards the old state.
	PredictorVersion string
}

// NewConfig returns a Config populated with the Python dataclass defaults.
func NewConfig() Config {
	return Config{
		CacheDir:         DefaultCacheDir(),
		LambdaL2:         1.0,
		MaxHistoryRows:   500,
		TrainingCadence:  "per-capture",
		PredictorVersion: DefaultPredictorVersion,
	}
}

// withDefaults fills any zero-valued field with its documented default.
func (c Config) withDefaults() Config {
	if c.CacheDir == "" {
		c.CacheDir = DefaultCacheDir()
	}
	if c.LambdaL2 == 0 {
		c.LambdaL2 = 1.0
	}
	if c.MaxHistoryRows == 0 {
		c.MaxHistoryRows = 500
	}
	if c.TrainingCadence == "" {
		c.TrainingCadence = "per-capture"
	}
	if c.PredictorVersion == "" {
		c.PredictorVersion = DefaultPredictorVersion
	}
	return c
}

// FeatureVector materialises the fixed-dim feature vector the sidecar consumes.
//
// Order is load-bearing — it pins the column index of every weight in the ridge
// fit. Changing it requires bumping SchemaVersion (older saved states would
// otherwise silently align mismatched columns to the wrong feature). The
// leading 1.0 is the bias / intercept term.
func FeatureVector(f predictor.ShotFeatures, crf int) []float64 {
	return []float64{
		1.0, // bias term
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
}

// HistoryRow is one entry in the drift-detection ring buffer.
type HistoryRow struct {
	Residual      float64 `json:"residual"`
	PredictedVMAF float64 `json:"predicted_vmaf"`
	ObservedVMAF  float64 `json:"observed_vmaf"`
}

// Model holds the online-ridge bias-correction state.
//
// It maintains a weight vector w and the inverse Gram matrix AInv such that
// w = AInv @ (Xᵀy) for all observed (features, residual) pairs. A closed-form
// rank-1 Sherman-Morrison update keeps both quantities in sync per capture in
// O(d²) time without any numerical solver.
type Model struct {
	Config   Config
	Weights  []float64
	AInv     [][]float64
	History  []HistoryRow
	NUpdates int
}

// NewModel returns a cold-start model: zero weights and AInv = (1/lambda)·I.
func NewModel(cfg Config) *Model {
	cfg = cfg.withDefaults()
	return &Model{
		Config:  cfg,
		Weights: make([]float64, FeatureDim),
		AInv:    eyeScaled(FeatureDim, 1.0/cfg.LambdaL2),
	}
}

// eyeScaled returns scale·I as a d×d matrix.
func eyeScaled(d int, scale float64) [][]float64 {
	m := make([][]float64, d)
	for i := range m {
		m[i] = make([]float64, d)
		m[i][i] = scale
	}
	return m
}

// matvec returns matrix @ vec.
func matvec(matrix [][]float64, vec []float64) []float64 {
	out := make([]float64, len(matrix))
	for i, row := range matrix {
		var s float64
		for j, v := range vec {
			s += row[j] * v
		}
		out[i] = s
	}
	return out
}

// dot returns a · b.
func dot(a, b []float64) float64 {
	var s float64
	for i := range a {
		s += a[i] * b[i]
	}
	return s
}

// outerAXPY applies matrix += alpha * outer(u, v) in place.
func outerAXPY(matrix [][]float64, u, v []float64, alpha float64) {
	for i, ui := range u {
		scale := alpha * ui
		if scale == 0.0 {
			continue
		}
		row := matrix[i]
		for j, vj := range v {
			row[j] += scale * vj
		}
	}
}

// Update folds one (features, residual) pair into the ridge fit.
//
// The residual is y = observedVMAF − predictedVMAF; the sidecar learns to
// predict that residual from features. The Sherman-Morrison rank-1 inverse
// update keeps AInv consistent with the cumulative Gram matrix without a solve.
func (m *Model) Update(f predictor.ShotFeatures, observedVMAF, predictedVMAF float64, crf int) {
	x := FeatureVector(f, crf)
	residual := observedVMAF - predictedVMAF

	// Sherman-Morrison: AInv' = AInv - (AInv x xᵀ AInv) / (1 + xᵀ AInv x).
	aInvX := matvec(m.AInv, x)
	denom := 1.0 + dot(x, aInvX)
	// Numerical safety: denom is positive whenever lambda > 0 and the
	// feature vector is finite. Guard against pathological inputs (NaN,
	// +Inf in features) by skipping the update.
	if math.IsNaN(denom) || math.IsInf(denom, 0) || denom <= 0.0 {
		return
	}
	outerAXPY(m.AInv, aInvX, aInvX, -1.0/denom)

	// Weight update: w' = w + (residual − xᵀw)/denom · AInv x, the standard
	// recursive least-squares form on the centred residual.
	predictionError := residual - dot(x, m.Weights)
	coeff := predictionError / denom
	// Re-derive AInv x against the updated AInv for stability.
	aInvXNew := matvec(m.AInv, x)
	for i := 0; i < FeatureDim; i++ {
		m.Weights[i] += coeff * aInvXNew[i]
	}

	// Ring buffer for drift detection.
	m.History = append(m.History, HistoryRow{
		Residual:      residual,
		PredictedVMAF: predictedVMAF,
		ObservedVMAF:  observedVMAF,
	})
	if len(m.History) > m.Config.MaxHistoryRows {
		m.History = m.History[len(m.History)-m.Config.MaxHistoryRows:]
	}
	m.NUpdates++
}

// PredictCorrection returns the additive correction in VMAF units. Cold start
// (no captures) has zero weights, so this returns exactly 0.0.
func (m *Model) PredictCorrection(f predictor.ShotFeatures, crf int) float64 {
	return dot(m.Weights, FeatureVector(f, crf))
}

// RecentResidualRMS is the RMS of the buffered residuals — the drift-detection
// signal. It returns 0.0 for an empty history (cold start).
func (m *Model) RecentResidualRMS() float64 {
	if len(m.History) == 0 {
		return 0.0
	}
	var s float64
	for _, row := range m.History {
		s += row.Residual * row.Residual
	}
	return math.Sqrt(s / float64(len(m.History)))
}

// state is the on-disk JSON layout. Field order matches the Python dict; the
// CLI's --json output sorts keys, so the wire order here is irrelevant to
// consumers but is kept aligned for diff-readability.
type state struct {
	SchemaVersion    int          `json:"schema_version"`
	PredictorVersion string       `json:"predictor_version"`
	FeatureDim       int          `json:"feature_dim"`
	LambdaL2         float64      `json:"lambda_l2"`
	Weights          []float64    `json:"weights"`
	AInv             [][]float64  `json:"a_inv"`
	History          []HistoryRow `json:"history"`
	NUpdates         int          `json:"n_updates"`
}

// toState serialises the model to its JSON-compatible shape.
func (m *Model) toState() state {
	history := m.History
	if history == nil {
		history = []HistoryRow{}
	}
	return state{
		SchemaVersion:    SchemaVersion,
		PredictorVersion: m.Config.PredictorVersion,
		FeatureDim:       FeatureDim,
		LambdaL2:         m.Config.LambdaL2,
		Weights:          m.Weights,
		AInv:             m.AInv,
		History:          history,
		NUpdates:         m.NUpdates,
	}
}

// modelFromState reconstructs a Model, rejecting a schema, dimensionality, or
// predictor-version mismatch. Callers (Load) discard the state on error and
// fall back to cold start.
func modelFromState(s state, cfg Config) (*Model, error) {
	if s.SchemaVersion != SchemaVersion {
		return nil, fmt.Errorf("sidecar schema version %d != %d", s.SchemaVersion, SchemaVersion)
	}
	if s.FeatureDim != FeatureDim {
		return nil, fmt.Errorf("sidecar feature_dim %d != %d", s.FeatureDim, FeatureDim)
	}
	if s.PredictorVersion != cfg.PredictorVersion {
		return nil, fmt.Errorf("sidecar predictor_version %q != %q",
			s.PredictorVersion, cfg.PredictorVersion)
	}
	if len(s.Weights) != FeatureDim || len(s.AInv) != FeatureDim {
		return nil, fmt.Errorf("sidecar state has wrong shape")
	}
	history := s.History
	if history == nil {
		history = []HistoryRow{}
	}
	return &Model{
		Config:   cfg,
		Weights:  s.Weights,
		AInv:     s.AInv,
		History:  history,
		NUpdates: s.NUpdates,
	}, nil
}

// SchemaVersionOf reports the schema version the model serialises as. It exists
// so the CLI's status payload can read the same value the on-disk state
// carries without re-marshalling.
func (m *Model) SchemaVersionOf() int { return SchemaVersion }

// Save writes the state to path as JSON, creating parent directories.
func (m *Model) Save(path string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("create sidecar state dir: %w", err)
	}
	data, err := json.MarshalIndent(m.toState(), "", "  ")
	if err != nil {
		return fmt.Errorf("marshal sidecar state: %w", err)
	}
	// 0o600: the state carries per-host training residuals; restrict to the
	// owner.
	if err := os.WriteFile(path, append(data, '\n'), 0o600); err != nil {
		return fmt.Errorf("write sidecar state: %w", err)
	}
	return nil
}

// Load reads state from path or returns a cold-start model.
//
// A predictor-version or schema-version mismatch returns a cold-start model
// rather than an error. Corrupted JSON also returns cold start; the corrupted
// file is left in place so the operator can inspect it.
func Load(path string, cfg Config) *Model {
	cfg = cfg.withDefaults()
	info, err := os.Stat(path)
	if err != nil || info.IsDir() {
		return NewModel(cfg)
	}
	data, err := os.ReadFile(path) // #nosec G304 -- path is derived from the operator-supplied cache dir.
	if err != nil {
		return NewModel(cfg)
	}
	var s state
	if err := json.Unmarshal(data, &s); err != nil {
		return NewModel(cfg)
	}
	m, err := modelFromState(s, cfg)
	if err != nil {
		return NewModel(cfg)
	}
	return m
}

// hostUUIDPath is the persisted random host UUID. It lives at the cache-dir
// root (above per-predictor-version subdirectories) so it survives predictor
// upgrades.
func hostUUIDPath(cacheDir string) string {
	return filepath.Join(cacheDir, "host-uuid")
}

// GetOrCreateHostUUID returns the anonymous random host UUID, creating it on
// first call.
//
// The UUID is a 32-character hex string (128 bits of entropy) drawn from a
// cryptographic RNG. It is never derived from MAC, hostname, /etc/machine-id,
// CPUID, or any other machine-identifying signal — a load-bearing precondition
// for the future opt-in upload path (ADR-0325 §Future work).
func GetOrCreateHostUUID(cacheDir string) (string, error) {
	if err := os.MkdirAll(cacheDir, 0o750); err != nil {
		return "", fmt.Errorf("create sidecar cache dir: %w", err)
	}
	path := hostUUIDPath(cacheDir)
	if info, err := os.Stat(path); err == nil && !info.IsDir() {
		if data, readErr := os.ReadFile(path); readErr == nil { // #nosec G304 -- operator-supplied cache dir.
			if uuid := trimSpace(string(data)); uuid != "" {
				return uuid, nil
			}
		}
		// Fall through to regeneration; the existing file is overwritten.
	}
	buf := make([]byte, 16)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("generate host uuid: %w", err)
	}
	uuid := hex.EncodeToString(buf)
	// Atomic-ish: write to a temp sibling then rename.
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(uuid+"\n"), 0o600); err != nil {
		return "", fmt.Errorf("write host uuid: %w", err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return "", fmt.Errorf("install host uuid: %w", err)
	}
	return uuid, nil
}

// trimSpace strips ASCII whitespace from both ends, matching str.strip() for
// the character set a hex token can be surrounded by.
func trimSpace(s string) string {
	start := 0
	for start < len(s) && isSpace(s[start]) {
		start++
	}
	end := len(s)
	for end > start && isSpace(s[end-1]) {
		end--
	}
	return s[start:end]
}

func isSpace(b byte) bool {
	switch b {
	case ' ', '\t', '\n', '\r', '\v', '\f':
		return true
	}
	return false
}

// Predictor composes a base predictor with a sidecar Model.
//
// PredictVMAF delegates to the wrapped predictor then adds the model's
// correction. Callers feed captured (features, observedVMAF) pairs back via
// RecordCapture, which also persists state to the cache dir.
type Predictor struct {
	Base      *predictor.Predictor
	Model     *Model
	Codec     string
	StatePath string
	HostUUID  string
}

// StatePathFor returns the canonical per-codec state path under cfg's cache
// dir.
func StatePathFor(cfg Config, codec string) string {
	cfg = cfg.withDefaults()
	return filepath.Join(cfg.CacheDir, cfg.PredictorVersion, codec, "state.json")
}

// ForCodec wires a sidecar for base against codec.
//
// It loads existing state from the canonical cache path when one exists and
// its recorded predictor version matches; otherwise it starts cold.
func ForCodec(base *predictor.Predictor, codec string, cfg Config) (*Predictor, error) {
	cfg = cfg.withDefaults()
	hostUUID := cfg.HostUUID
	if hostUUID == "" {
		var err error
		hostUUID, err = GetOrCreateHostUUID(cfg.CacheDir)
		if err != nil {
			return nil, err
		}
	}
	statePath := StatePathFor(cfg, codec)
	return &Predictor{
		Base:      base,
		Model:     Load(statePath, cfg),
		Codec:     codec,
		StatePath: statePath,
		HostUUID:  hostUUID,
	}, nil
}

// PredictVMAF returns the base prediction plus the sidecar correction, clamped
// to [0, 100] so a runaway correction cannot push the score outside the VMAF
// range.
func (p *Predictor) PredictVMAF(f predictor.ShotFeatures, crf int, codec string) float64 {
	c := codec
	if c == "" {
		c = p.Codec
	}
	base := p.Base.PredictVMAF(f, crf, c)
	correction := p.Model.PredictCorrection(f, crf)
	return predictor.Clamp(base+correction, 0.0, 100.0)
}

// RecordCapture folds one observed VMAF into the sidecar fit.
//
// It computes the bare-predictor prediction (sidecar excluded) against
// features / crf / codec, then folds the residual observed − predicted into the
// ridge state. It saves to disk unless persist is false.
func (p *Predictor) RecordCapture(
	f predictor.ShotFeatures, crf int, observedVMAF float64, codec string, persist bool,
) error {
	c := codec
	if c == "" {
		c = p.Codec
	}
	predicted := p.Base.PredictVMAF(f, crf, c)
	p.Model.Update(f, observedVMAF, predicted, crf)
	if persist {
		return p.Save()
	}
	return nil
}

// Save persists the sidecar state to the configured cache path.
func (p *Predictor) Save() error { return p.Model.Save(p.StatePath) }
