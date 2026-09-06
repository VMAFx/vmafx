// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/libvmaf.go — Go wrapper around the libvmaf C ABI via cgo.
//
// Local-dev linking strategy (CPU-only build):
//
//	cgo LDFLAGS: -L${SRCDIR}/../../core/build-cpu/src -lvmaf
//
// Production / container linking strategy:
//
//	libvmaf.so is installed at /usr/local/lib.
//	Set LD_LIBRARY_PATH=/usr/local/lib (or run ldconfig) before starting
//	the binary.  The linker flag below covers both cases via -lvmaf:
//	  CGO_LDFLAGS="-L/usr/local/lib -lvmaf" or LD_LIBRARY_PATH at run-time.
//
// The cgo compiler flag `-I${SRCDIR}/../../core/include` resolves the public
// libvmaf header from the fork tree (core/include/libvmaf/libvmaf.h).
// In the distroless production image libvmaf headers are not required at
// run-time — only libvmaf.so matters.
//
// The Scorer interface is intentionally thin: New → Score → Close.
// Extensions (e.g. batch scoring, per-frame callbacks) belong in a
// separate package built on top of this one.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package libvmaf

/*
#cgo CFLAGS: -I${SRCDIR}/../../core/include
#cgo LDFLAGS: -L${SRCDIR}/../../core/build-cpu/src -lvmaf -lm

#include <libvmaf/libvmaf.h>
#include <stdlib.h>

// cgoScore is a C helper that invokes the vmaf binary as a subprocess.
// We use the binary path approach rather than direct libvmaf API calls here
// because the full pipeline (picture allocation, feature extraction, pooling)
// would require reimplementing vmaf.c in cgo — that is significant scope for
// a first-generation wrapper.  The binary-delegation approach is identical to
// what the Python layer does today and is safe: the binary runs in a separate
// process with its own memory space.
//
// The direct cgo path (vmaf_init → vmaf_use_features_from_model_file →
// vmaf_read_pictures → vmaf_score_pooled → vmaf_close) is left as a
// forward-compatible extension point — see ScoreDirect below.
*/
import "C"

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/VMAFx/vmafx/pkg/cliopt"
	"github.com/VMAFx/vmafx/pkg/model"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
	"unsafe"
)

// Ensure C import is used (avoids "imported and not used" errors when build
// tags exclude cgo usage at link time).
var _ = unsafe.Sizeof(0)

// Scorer is the public Go API for VMAF scoring.
type Scorer struct {
	// binaryPath is the path to the vmaf CLI binary.
	binaryPath string
	// modelDir is the directory searched for .json model files.
	modelDir string
}

// New creates a Scorer backed by the vmaf CLI binary at binaryPath.
// modelDir is the directory containing VMAF .json model files; pass "" to
// use the binary's compiled-in model search path.
func New(binaryPath, modelDir string) (*Scorer, error) {
	if binaryPath == "" {
		// Fall back to PATH.
		var err error
		binaryPath, err = exec.LookPath("vmaf")
		if err != nil {
			return nil, fmt.Errorf("libvmaf: vmaf binary not found on PATH: %w", err)
		}
	}
	if _, err := os.Stat(binaryPath); err != nil {
		return nil, fmt.Errorf("libvmaf: vmaf binary not accessible at %s: %w", binaryPath, err)
	}
	return &Scorer{binaryPath: binaryPath, modelDir: modelDir}, nil
}

// Score computes the VMAF score for the reference/distorted pair.
// ref and dis must be valid paths to YUV or Y4M files readable by the vmaf binary.
// modelName selects the VMAF model (e.g. "vmaf_v0.6.1"); pass "" for the default.
//
// Score shells out to the vmaf CLI and parses its JSON output.
//
// The supplied ctx governs the lifetime of the underlying vmaf subprocess via
// exec.CommandContext: when ctx is cancelled (deadline elapsed or client
// disconnected) the subprocess receives SIGKILL and any in-flight cgo work is
// abandoned.  Callers in long-running services (HTTP / gRPC handlers) MUST
// pass the request-scoped context so subprocesses do not outlive the request.
//
// Background-style callers may pass context.Background() to opt out of
// cancellation; passing a nil ctx is treated as context.Background() but
// raises ErrInvalidArgument from downstream callers that require a real
// cancellation signal (see T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31).
func (s *Scorer) Score(ctx context.Context, ref, dis, modelName string) (float64, map[string]float64, error) {
	if ctx == nil {
		// Defensive: callers should pass a real context, but nil ctx would
		// otherwise panic inside exec.CommandContext.
		ctx = context.Background()
	}
	if modelName == "" {
		modelName = model.DefaultVersion
	}

	// Honour an already-cancelled context up-front so we don't allocate temp
	// files / fork a subprocess only to immediately tear them down.
	if err := ctx.Err(); err != nil {
		return 0, nil, fmt.Errorf("libvmaf: context cancelled before Score: %w", err)
	}

	// Locate the model file.
	modelPath, err := s.resolveModel(modelName)
	if err != nil {
		return 0, nil, err
	}

	// Write output JSON to a temp file so we can parse it.
	tmpOut, err := os.CreateTemp("", "vmafx-score-*.json")
	if err != nil {
		return 0, nil, fmt.Errorf("libvmaf: create temp output: %w", err)
	}
	if closeErr := tmpOut.Close(); closeErr != nil {
		return 0, nil, fmt.Errorf("libvmaf: close temp output: %w", closeErr)
	}
	defer func() {
		if rmErr := os.Remove(tmpOut.Name()); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
			slog.Warn("libvmaf: remove temp output", "error", rmErr)
		}
	}()

	args := []string{
		"-r", ref,
		"-d", dis,
		// ADR-1190: the CLI splits option strings on ":" and "=", so a model
		// path containing either has to be escaped or it is truncated/rejected.
		"-m", "path=" + cliopt.EscapeValue(modelPath),
		"-o", tmpOut.Name(),
		"--json",
	}

	// exec.CommandContext wires SIGKILL to ctx.Done() so a cancelled context
	// (client disconnect, deadline elapsed, parent shutdown) tears down the
	// subprocess instead of leaving it running with the file descriptors of
	// the dropped request.  Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
	//
	// WaitDelay enforces a hard upper bound on how long cmd.Run() blocks
	// after the context is cancelled.  Without it, Go waits for every
	// inherited file descriptor in the child (including those held by
	// grandchildren the vmaf binary may have forked) to close, which can
	// hang indefinitely.  After WaitDelay elapses, Go closes the I/O
	// pipes and returns; the underlying SIGKILL ensures the kernel will
	// reap the children eventually.  2 s is generous: in practice the
	// kernel completes process teardown in single-digit milliseconds.
	cmd := exec.CommandContext(ctx, s.binaryPath, args...) //nolint:gosec // paths are operator-supplied
	cmd.WaitDelay = 2 * time.Second
	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	if err := cmd.Run(); err != nil {
		// Surface the cancellation cause when ctx was the trigger so callers
		// can distinguish "vmaf binary genuinely failed" from "we killed it
		// because the request went away".
		if ctxErr := ctx.Err(); ctxErr != nil {
			return 0, nil, fmt.Errorf("libvmaf: vmaf subprocess cancelled: %w (run err: %v, stderr: %s)",
				ctxErr, err, stderr.String())
		}
		return 0, nil, fmt.Errorf("libvmaf: vmaf binary failed: %w\nstderr: %s", err, stderr.String())
	}

	return parseOutput(tmpOut.Name())
}

// Close releases any resources held by the Scorer.
// Currently a no-op (the binary-delegation model creates no persistent
// resources), but present for interface symmetry with the direct-cgo path.
func (s *Scorer) Close() {}

// ResolveModel turns a VMAF model name (e.g. "vmaf_v0.6.1") or an absolute
// model-file path into an absolute path to a model JSON readable by the
// in-process StreamScorer.  Pass "" to select the default model.
//
// This exported wrapper lets the gRPC ScoreStream handler (ADR-0933 Phase 2)
// resolve the proto StreamConfig.model field to a concrete ModelPath without
// duplicating the search-order logic that the subprocess Score path already
// implements.
func (s *Scorer) ResolveModel(name string) (string, error) {
	if name == "" {
		name = model.DefaultVersion
	}
	return s.resolveModel(name)
}

// resolveModel returns the absolute path to a VMAF model JSON file.
// Search order: explicit path → modelDir/<name>.json → embedded model dir.
func (s *Scorer) resolveModel(name string) (string, error) {
	// If name is already an absolute path that exists, use it directly.
	if filepath.IsAbs(name) {
		if _, err := os.Stat(name); err == nil {
			return name, nil
		}
	}

	// Try modelDir.
	if s.modelDir != "" {
		candidate := filepath.Join(s.modelDir, name+".json")
		if _, err := os.Stat(candidate); err == nil {
			return candidate, nil
		}
		// Also try without appending .json in case name already ends with it.
		candidate = filepath.Join(s.modelDir, name)
		if _, err := os.Stat(candidate); err == nil {
			return candidate, nil
		}
		// The v1.0.16 models are not flat in model/: they live in a
		// per-family subdirectory (model/vmaf_v1.0.16/... and
		// model/vmaf_v1.0.16_hfr/...). Without this branch the fork's own
		// default, vmaf_v1.0.16_3d0h, does not resolve.
		if family := modelFamilyDir(name); family != "" {
			candidate = filepath.Join(s.modelDir, family, name+".json")
			if _, err := os.Stat(candidate); err == nil {
				return candidate, nil
			}
		}
	}

	return "", fmt.Errorf("libvmaf: model %q not found (modelDir=%q)", name, s.modelDir)
}

// modelFamilyDir returns the model/ subdirectory a version string lives in, or
// "" when the model is stored flat. Only the v1.0.16 families are nested; the
// v0.6.1-era models sit directly in model/.
func modelFamilyDir(name string) string {
	switch {
	case strings.HasPrefix(name, "vmaf_v1.0.16_hfr_"):
		return "vmaf_v1.0.16_hfr"
	case strings.HasPrefix(name, "vmaf_v1.0.16_"):
		return "vmaf_v1.0.16"
	default:
		return ""
	}
}

// vmafJSONOutput represents the relevant subset of the vmaf CLI JSON output.
type vmafJSONOutput struct {
	Frames []struct {
		Metrics map[string]float64 `json:"metrics"`
	} `json:"frames"`
	PooledMetrics struct {
		VMAF struct {
			Mean float64 `json:"mean"`
		} `json:"vmaf"`
		// Additional per-feature pooled values — arbitrary keys.
		// We capture them via a second pass below.
	} `json:"pooled_metrics"`
	AggregateMetrics map[string]struct {
		Mean float64 `json:"mean"`
	} `json:"aggregate_metrics"`
}

// parseOutput reads the vmaf JSON output file and returns the aggregate score
// and per-feature map.
func parseOutput(path string) (float64, map[string]float64, error) {
	data, err := os.ReadFile(path) //nolint:gosec // path is our own tmpfile
	if err != nil {
		return 0, nil, fmt.Errorf("libvmaf: read output file: %w", err)
	}

	// The vmaf JSON schema uses "pooled_metrics" for aggregate statistics.
	// Parse as a generic map to handle arbitrary feature keys.
	var raw struct {
		PooledMetrics map[string]struct {
			Mean float64 `json:"mean"`
		} `json:"pooled_metrics"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return 0, nil, fmt.Errorf("libvmaf: parse JSON output: %w", err)
	}

	features := make(map[string]float64, len(raw.PooledMetrics))
	for k, v := range raw.PooledMetrics {
		features[strings.ToLower(k)] = v.Mean
	}

	score, ok := features["vmaf"]
	if !ok {
		return 0, nil, fmt.Errorf("libvmaf: 'vmaf' key not found in pooled_metrics; output: %s", string(data))
	}

	return score, features, nil
}
