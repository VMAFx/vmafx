// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/readiness.go — readiness checks that look at the host, not
// just at the process. A scorer that was constructed at start-up is no proof
// that the vmaf binary it shells out to is still there: a replaced image layer,
// an unmounted volume or a chmod makes every Score fail while /readyz stays 200.
// Issue #1251 (health / readiness endpoints).

//go:build cgo

package main

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"os/exec"

	"github.com/golusoris/golusoris/core/config"
	"github.com/golusoris/golusoris/k8s/health"
	"github.com/golusoris/golusoris/observability/statuspage"

	"github.com/VMAFx/vmafx/internal/app/scoringservice"
)

// executableBits is the permission mask a runnable vmaf binary must intersect.
const executableBits os.FileMode = 0o111

// registerBinaryReadiness adds the readiness check "vmaf-binary": the binary
// named by vmaf.binary (or `vmaf` on PATH) must exist and be an executable
// regular file, and model.dir, when set, must be a directory.
func registerBinaryReadiness(reg *statuspage.Registry, cfg *config.Config) {
	binary := cfg.Get("vmaf.binary")
	modelDir := cfg.Get("model.dir")
	reg.Register(statuspage.Check{
		Name: "vmaf-binary",
		Tags: []string{health.TagReadiness},
		Fn: func(ctx context.Context) error {
			return checkScorerHost(ctx, binary, modelDir)
		},
	})
}

// checkScorerHost verifies the files the scorer needs right now. It performs
// only stat calls, so it is bounded; the context is honoured before them.
func checkScorerHost(ctx context.Context, binary, modelDir string) error {
	if err := ctx.Err(); err != nil {
		return fmt.Errorf("readiness check cancelled: %w", err)
	}
	path := binary
	if path == "" {
		found, err := exec.LookPath("vmaf")
		if err != nil {
			return fmt.Errorf("vmaf binary not found on PATH: %w", err)
		}
		path = found
	}
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("vmaf binary not accessible: %w", err)
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&executableBits == 0 {
		return fmt.Errorf("vmaf binary %s is not an executable file", path)
	}
	if modelDir == "" {
		return nil
	}
	dir, err := os.Stat(modelDir)
	if err != nil {
		return fmt.Errorf("model directory not accessible: %w", err)
	}
	if !dir.IsDir() {
		return fmt.Errorf("model directory %s is not a directory", modelDir)
	}
	return nil
}

// registryReady reports whether every readiness check of reg is up or
// degraded, with the same rule the golusoris /readyz handler applies.
func registryReady(ctx context.Context, reg *statuspage.Registry) bool {
	if reg == nil {
		return true
	}
	for _, r := range reg.RunTagged(ctx, health.TagReadiness) {
		if r.Status != statuspage.StatusUp && r.Status != statuspage.StatusDegraded {
			return false
		}
	}
	return true
}

// handleReadyzFromRegistry serves the legacy JSON /readyz contract, ready only
// while the scorer exists and every registered readiness check passes.
func (h *httpServer) handleReadyzFromRegistry(reg *statuspage.Registry) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ready := h.scorer != nil && registryReady(r.Context(), reg)
		scoringservice.HandleReadyz(h.metrics, h.log, ready, w, r)
	}
}
