// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package libvmaf

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/VMAFx/vmafx/pkg/cliopt"
	"github.com/VMAFx/vmafx/pkg/model"
)

// Request is one scoring run of the vmaf CLI with options (#2155).
type Request struct {
	// Reference and Distorted are the input paths.
	Reference, Distorted string
	// Model is a model name or an absolute model path; "" selects the library
	// default (model.DefaultVersion).
	Model string
	// ModelSuffix is appended to the model specification, for example
	// ":disable_clip:adm.adm_norm_view_dist=4" (pkg/scoreopts.ModelSpec).
	ModelSuffix string
	// Args are further vmaf flags, built from the scoring options by
	// pkg/scoreopts; never spelled by the caller.
	Args []string
}

// FeatureBackend is one entry of the report's backend receipt.
type FeatureBackend struct {
	Extractor string `json:"extractor"`
	Backend   string `json:"backend"`
}

// Result is what a run reports: the pooled score and features, the model it
// loaded, and the report's provenance (#2142, #2155).
type Result struct {
	Score           float64
	Features        map[string]float64
	ModelName       string
	ModelPath       string
	ModelSHA256     string
	Version         string
	BackendUsed     string
	FeatureBackends []FeatureBackend
	// Provenance is the report's "provenance" object: one key per field of
	// VmafxProvenance, read into the proto Provenance message by the server.
	Provenance json.RawMessage
}

// maxModelBytes bounds the model file hashed for the provenance record.
const maxModelBytes = 256 << 20

// Run scores one pair with the request's options and returns the parsed
// report. The supplied ctx governs the vmaf subprocess as in Score.
func (s *Scorer) Run(ctx context.Context, req Request) (*Result, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := ctx.Err(); err != nil {
		return nil, fmt.Errorf("libvmaf: context cancelled before Run: %w", err)
	}
	name := req.Model
	if name == "" {
		name = model.DefaultVersion
	}
	modelPath, err := s.resolveModel(name)
	if err != nil {
		return nil, err
	}
	digest, err := fileSHA256(modelPath)
	if err != nil {
		return nil, err
	}
	out, removeOut, err := scoreOutputFile()
	if err != nil {
		return nil, err
	}
	defer removeOut()

	runCtx, cancel := context.WithTimeout(ctx, scoreBudget(ctx.Deadline()))
	defer cancel()
	if err := s.runScoreBinary(runCtx, runArgv(req, modelPath, out)); err != nil {
		return nil, err
	}
	result, err := parseReport(out)
	if err != nil {
		return nil, err
	}
	result.ModelName, result.ModelPath, result.ModelSHA256 = name, modelPath, digest
	return result, nil
}

// runArgv is the vmaf argument vector of a run: the inputs, the resolved model
// with its suffix, a JSON report, then the request's flags.
//
// ADR-1190: the CLI splits option strings on ":" and "=", so the model path is
// escaped; the suffix is the caller's option syntax and is passed as is.
func runArgv(req Request, modelPath, outPath string) []string {
	argv := []string{
		"-r", req.Reference,
		"-d", req.Distorted,
		"-m", "path=" + cliopt.EscapeValue(modelPath) + req.ModelSuffix,
		"-o", outPath,
		"--json",
	}
	return append(argv, req.Args...)
}

// fileSHA256 is the lower-case hex SHA-256 of a model file.
func fileSHA256(path string) (digest string, err error) {
	file, err := os.Open(path) //nolint:gosec // the path came from resolveModel
	if err != nil {
		return "", fmt.Errorf("libvmaf: open model for hashing: %w", err)
	}
	defer func() {
		if closeErr := file.Close(); closeErr != nil && err == nil {
			digest, err = "", fmt.Errorf("libvmaf: close model after hashing: %w", closeErr)
		}
	}()
	hash := sha256.New()
	if _, err := io.Copy(hash, io.LimitReader(file, maxModelBytes)); err != nil {
		return "", fmt.Errorf("libvmaf: hash model: %w", err)
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

// vmafReport is the subset of the vmaf JSON report a Result carries.
type vmafReport struct {
	Version       string `json:"version"`
	PooledMetrics map[string]struct {
		Mean float64 `json:"mean"`
	} `json:"pooled_metrics"`
	BackendUsed     string           `json:"backend_used"`
	FeatureBackends []FeatureBackend `json:"feature_backends"`
	Provenance      json.RawMessage  `json:"provenance"`
}

// parseReport reads the vmaf JSON report at path.
func parseReport(path string) (*Result, error) {
	data, err := os.ReadFile(path) //nolint:gosec // path is our own tmpfile
	if err != nil {
		return nil, fmt.Errorf("libvmaf: read output file: %w", err)
	}
	var report vmafReport
	if err := json.Unmarshal(data, &report); err != nil {
		return nil, fmt.Errorf("libvmaf: parse JSON output: %w", err)
	}
	features := make(map[string]float64, len(report.PooledMetrics))
	for name, pooled := range report.PooledMetrics {
		features[strings.ToLower(name)] = pooled.Mean
	}
	score, ok := features["vmaf"]
	if !ok {
		return nil, fmt.Errorf("libvmaf: 'vmaf' key not found in pooled_metrics; output: %s", string(data))
	}
	return &Result{
		Score:           score,
		Features:        features,
		Version:         report.Version,
		BackendUsed:     report.BackendUsed,
		FeatureBackends: report.FeatureBackends,
		Provenance:      report.Provenance,
	}, nil
}
