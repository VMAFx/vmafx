// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// impl_direct.go contains the direct-cgo handler variants that run when
// VMAFX_MCP_DIRECT=1 is set in the environment.  See
// docs/adr/0931-mcp-cgo-direct-replace-subprocess.md for the rollout plan.
//
// Phase 1 covers two tools:
//   - vmaf_score      → runVmafScoreDirect
//   - describe_model  → describeModelDirect (validates via libvmaf in addition
//                       to the file-only parse the subprocess path uses).
//
// The subprocess implementations in impl.go remain the default and the
// fallback when libvmaf rejects an input the direct path cannot handle
// (e.g. DNN models, GPU backends in Phase 1).

package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// directPathEnabled reports whether the operator opted into the direct cgo
// scoring path via VMAFX_MCP_DIRECT=1.  Any other value (including unset or
// "0") returns false.
func directPathEnabled() bool {
	return os.Getenv("VMAFX_MCP_DIRECT") == "1"
}

// runVmafScoreDirect is the direct-cgo replacement for runVmafScore.
//
// Returns a payload shaped like the subprocess path's JSON output so MCP
// clients (Claude Desktop, Cursor, the gRPC server) see byte-compatible
// responses.  The "backend_used" field reports "cpu (direct cgo)" so
// operators can tell from a single call whether the direct path took effect.
//
// Phase 1 limitations:
//   - CPU backend only.  A non-"auto"/"cpu" backend request falls back to
//     the subprocess path (so existing GPU dispatch still works).
//   - SVM models only (.json / .pkl).  ONNX/DNN models route to subprocess
//     until Phase 3 adds the cgo ONNX bridge.
func runVmafScoreDirect(ctx context.Context, ref, dis string, width, height int, pixfmt string, bitdepth int, modelArg, backend string) (map[string]any, error) {
	// GPU backends require Phase 2; fall back transparently.
	if backend != "auto" && backend != "cpu" {
		return runVmafScore(ctx, ref, dis, width, height, pixfmt, bitdepth, modelArg, backend, "17")
	}

	// Resolve the model arg.  The MCP schema accepts "version=vmaf_v0.6.1"
	// or "path=/abs/path"; the subprocess path forwards the string verbatim
	// to the vmaf CLI.  For the direct path we have to materialise the
	// file path so vmaf_model_load_from_path can open it.
	modelPath, err := resolveModelArgToPath(modelArg)
	if err != nil {
		// Unresolvable model -> fall back to subprocess (it has its own
		// "version=..." resolver inside vmaf.c).
		return runVmafScore(ctx, ref, dis, width, height, pixfmt, bitdepth, modelArg, backend, "17")
	}

	// DNN / .onnx models are out of scope for Phase 1.
	if ext := strings.ToLower(filepath.Ext(modelPath)); ext == ".onnx" {
		return runVmafScore(ctx, ref, dis, width, height, pixfmt, bitdepth, modelArg, backend, "17")
	}

	pf, err := libvmaf.ParsePixFmt(pixfmt)
	if err != nil {
		return nil, err
	}

	libvmaf.LogDirectPathSelected()

	// Use the caller-scoped ctx so that a client disconnect cancels the cgo
	// scoring call, preventing orphan threads on the C side (ADR-1085).
	res, err := libvmaf.ScoreDirect(ctx, libvmaf.ScoreDirectRequest{
		Ref:       ref,
		Dis:       dis,
		ModelPath: modelPath,
		Width:     width,
		Height:    height,
		PixFmt:    pf,
		BitDepth:  bitdepth,
	})
	if err != nil {
		return nil, fmt.Errorf("ScoreDirect: %w", err)
	}

	payload := map[string]any{
		"pooled_metrics": map[string]any{
			"vmaf": map[string]any{
				"mean": res.VMAF,
			},
		},
		"frames":            []any{},
		"backend_requested": backend,
		"backend_used":      res.Backend + " (direct cgo)",
		"frame_count":       res.FrameCount,
	}
	if w := resolutionMismatchWarning(modelArg, width, height); w != "" {
		payload["mismatched_model_warning"] = w
	}
	return payload, nil
}

// resolveModelArgToPath converts the MCP-level model argument into an
// absolute path on disk.  The MCP schema's accepted forms are:
//
//   - "version=vmaf_v0.6.1"  → repoRoot/model/vmaf_v0.6.1.json
//   - "path=/abs/path.json"  → /abs/path.json
//   - "vmaf_v0.6.1"          → repoRoot/model/vmaf_v0.6.1.json (bare stem)
//   - "/abs/path.json"       → /abs/path.json
//
// Returns an error when no candidate file exists; the caller then falls back
// to the subprocess path so the legacy resolver inside vmaf.c gets a chance.
func resolveModelArgToPath(modelArg string) (string, error) {
	root := libvmaf.RepoRoot()
	modelsDir := filepath.Join(root, "model")

	// Strip "version=" or "path=" prefixes.
	stripped := modelArg
	if after, ok := strings.CutPrefix(stripped, "version="); ok {
		stripped = after
	} else if after, ok := strings.CutPrefix(stripped, "path="); ok {
		stripped = after
	}

	// Absolute / relative path that exists?
	if filepath.IsAbs(stripped) {
		if _, err := os.Stat(stripped); err == nil {
			return stripped, nil
		}
	} else if _, err := os.Stat(filepath.Join(root, stripped)); err == nil {
		return filepath.Join(root, stripped), nil
	}

	// Bare stem → look in model/.
	for _, ext := range []string{".json", ".pkl"} {
		candidate := filepath.Join(modelsDir, stripped+ext)
		if _, err := os.Stat(candidate); err == nil {
			return candidate, nil
		}
		// Already has the extension?
		if strings.HasSuffix(strings.ToLower(stripped), ext) {
			candidate = filepath.Join(modelsDir, stripped)
			if _, err := os.Stat(candidate); err == nil {
				return candidate, nil
			}
		}
	}
	return "", fmt.Errorf("model %q not resolvable to a local file", modelArg)
}

// describeModelDirect is the direct-cgo variant of describeModel.  It runs
// vmaf_model_load_from_path as a syntactic check (catches schema drift the
// pure-JSON parser silently tolerates), then returns the same payload the
// subprocess path produces.
//
// On any libvmaf load failure the function falls back to the file-only
// describeModel — the model may legitimately be a .pkl (libsvm pickle) or a
// .onnx file the file-only parser can still introspect.
func describeModelDirect(nameOrPath string) (map[string]any, error) {
	// Re-use the subprocess path for resolution + base payload assembly.
	base, err := describeModel(nameOrPath)
	if err != nil {
		return nil, err
	}

	pathField, ok := base["path"].(string)
	if !ok || pathField == "" {
		return base, nil
	}
	abs := pathField
	if !filepath.IsAbs(abs) {
		abs = filepath.Join(libvmaf.RepoRoot(), pathField)
	}
	if ext := strings.ToLower(filepath.Ext(abs)); ext != ".json" {
		// Only .json models can be parsed by the direct path validator
		// today; surface the subprocess-path payload unmodified.
		return base, nil
	}

	// Attempt a libvmaf load purely as a schema validator; success augments
	// the payload with a libvmaf_validated=true marker so the operator can
	// confirm the direct path was used.
	if err := validateModelViaLibvmaf(abs); err != nil {
		base["libvmaf_validated"] = false
		base["libvmaf_validation_error"] = err.Error()
	} else {
		base["libvmaf_validated"] = true
	}
	libvmaf.LogDirectPathSelected()
	return base, nil
}

// validateModelViaLibvmaf opens a model via the public libvmaf API and
// immediately destroys it.  Returns the typed error if the load fails.
//
// Implemented in direct.go's package, exported as ValidateModel below.
func validateModelViaLibvmaf(path string) error {
	return libvmaf.ValidateModel(path)
}
