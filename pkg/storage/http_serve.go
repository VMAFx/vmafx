// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/storage/http_serve.go — HTTPServeStorage implementation.
//
// HTTPServeStorage spawns a per-job "rclone serve http" subprocess, waits for
// it to be reachable, and returns an http://localhost:PORT/path URL that ffmpeg
// can consume directly via libavformat's HTTP demuxer.
//
// Lifecycle:
//  1. Prepare() finds a free TCP port.
//  2. Spawns: rclone serve http <remote:root> --addr 127.0.0.1:PORT [--config FILE]
//  3. Polls http://127.0.0.1:PORT/ until it returns HTTP 200/404 (rclone is ready).
//  4. Returns http://127.0.0.1:PORT/<relative-path>.
//  5. cleanup() sends SIGTERM to the rclone process and waits for exit.
//
// ADR-0719: vmafx-node rclone integration.
package storage

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/exec"
	"path"
	"strings"
	"time"
)

const (
	// serveReadyTimeout is the maximum time to wait for the rclone HTTP
	// server to become reachable after the subprocess starts.
	serveReadyTimeout = 15 * time.Second

	// serveReadyPollInterval is the interval between readiness polls.
	serveReadyPollInterval = 100 * time.Millisecond
)

// HTTPServeStorage streams remote assets via a per-job rclone HTTP server.
// It is the primary storage mode for vmafx-node because it supports multiple
// concurrent inputs (ref + dis) on the same serve instance when the remote
// root is set to the common bucket prefix.
type HTTPServeStorage struct {
	rcloneBin    string
	rcloneConfig string
	log          *slog.Logger
}

// Mode returns ModeHTTPServe.
func (s *HTTPServeStorage) Mode() Mode { return ModeHTTPServe }

// Prepare starts a "rclone serve http" subprocess for the remote root inferred
// from sourceURI, then returns an HTTP URL pointing at the specific asset path.
//
// The remote root is the portion of the rclone path up to the last "/"; the
// asset path is the filename. For example:
//
//	s3://bucket/prefix/ref.yuv  →  root = "s3:bucket/prefix"
//	                                asset = "/ref.yuv"
//	                                URL   = "http://127.0.0.1:PORT/ref.yuv"
func (s *HTTPServeStorage) Prepare(ctx context.Context, sourceURI string) (string, func(), error) {
	// Handle local paths without spawning rclone.
	if IsLocal(sourceURI) {
		lp, err := localPath(sourceURI)
		if err != nil {
			return "", func() {}, err
		}
		return lp, func() {}, nil
	}

	remotePath, err := rcloneRemotePath(sourceURI)
	if err != nil {
		return "", func() {}, err
	}

	// Split into root (served directory) and relative asset path.
	remoteRoot, assetRel := splitRemotePath(remotePath)

	// Find a free port.
	port, err := pickFreePort()
	if err != nil {
		return "", func() {}, err
	}

	addr := fmt.Sprintf("127.0.0.1:%d", port)
	argv := s.buildServeArgs(remoteRoot, addr)

	s.log.Debug("starting rclone serve http",
		"remote_root", remoteRoot,
		"addr", addr,
		"asset", assetRel,
	)

	// #nosec G204 -- argv[0] is s.rcloneBin (configured at construction);
	// argv[1:] mixes literals with remoteRoot (validated by caller) and addr
	// (formatted from a random free port + 127.0.0.1).
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.Stderr = os.Stderr

	if startErr := cmd.Start(); startErr != nil {
		return "", func() {}, fmt.Errorf("storage: start rclone serve: %w", startErr)
	}

	// Wait for the server to be ready. On timeout, surface both the
	// readiness failure and any kill-cleanup failure via errors.Join so
	// an orphaned rclone process does not get silently masked.
	if readyErr := waitForHTTP(ctx, addr, serveReadyTimeout); readyErr != nil {
		errs := []error{fmt.Errorf("storage: rclone serve did not become ready: %w", readyErr)}
		if killErr := killProcess(cmd); killErr != nil {
			errs = append(errs, fmt.Errorf("storage: kill rclone after readiness timeout: %w", killErr))
		}
		return "", func() {}, errors.Join(errs...)
	}

	readableURL := fmt.Sprintf("http://%s/%s", addr, strings.TrimPrefix(assetRel, "/"))

	cleanup := func() {
		if killErr := killProcess(cmd); killErr != nil {
			s.log.Warn("storage: kill rclone during cleanup", "error", killErr)
		}
	}

	s.log.Info("rclone serve http ready",
		"url", readableURL,
		"remote_root", remoteRoot,
	)

	return readableURL, cleanup, nil
}

// buildServeArgs constructs the rclone serve http argument list.
func (s *HTTPServeStorage) buildServeArgs(remoteRoot, addr string) []string {
	argv := []string{s.rcloneBin, "serve", "http", remoteRoot, "--addr", addr, "--no-checksum"}
	if s.rcloneConfig != "" {
		argv = append(argv, "--config", s.rcloneConfig)
	}
	return argv
}

// splitRemotePath splits a rclone remote:path string into the parent root
// (suitable as the rclone serve root) and the relative asset path.
//
// Examples:
//
//	"s3:bucket/prefix/ref.yuv"   → ("s3:bucket/prefix", "ref.yuv")
//	"s3:bucket/ref.yuv"          → ("s3:bucket", "ref.yuv")
//	"local:/data/ref.yuv"        → ("local:/data", "ref.yuv")
func splitRemotePath(remotePath string) (root, asset string) {
	// Find the last "/" in the path portion (after the "remote:" prefix).
	colonIdx := strings.Index(remotePath, ":")
	if colonIdx < 0 {
		// No colon → treat whole string as path.
		dir := path.Dir(remotePath)
		base := path.Base(remotePath)
		return dir, base
	}

	prefix := remotePath[:colonIdx+1] // e.g. "s3:"
	pathPart := remotePath[colonIdx+1:]

	dir := path.Dir(pathPart)
	base := path.Base(pathPart)

	if dir == "." || dir == "/" {
		return prefix, base
	}
	return prefix + dir, base
}

// waitForHTTP polls addr until rclone's HTTP server is reachable or timeout.
//
// The poll ticker is created once before the loop so each iteration reuses the
// same runtime timer — avoiding the per-iteration *time.Timer allocation that
// time.After() would create (SA1015 / ADR-1065).
func waitForHTTP(ctx context.Context, addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	client := &http.Client{Timeout: 2 * time.Second}
	tick := time.NewTicker(serveReadyPollInterval)
	defer tick.Stop()
	for {
		if time.Now().After(deadline) {
			return fmt.Errorf("timed out after %v waiting for http://%s/", timeout, addr)
		}
		resp, err := client.Get(fmt.Sprintf("http://%s/", addr))
		if err == nil {
			if closeErr := resp.Body.Close(); closeErr != nil {
				slog.Warn("storage: close readiness probe body", "error", closeErr)
			}
			// rclone returns 200 for directory listing or 404 for unknown path —
			// both indicate the server is accepting connections.
			if resp.StatusCode < 500 {
				return nil
			}
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-tick.C:
		}
	}
}

// killProcess sends SIGKILL to cmd and waits for it to exit. The kill failure
// is returned to the caller so multi-step cleanup paths can join it with their
// primary error; the post-kill Wait() error is intentionally not returned
// because it is expected to be "signal: killed".
func killProcess(cmd *exec.Cmd) error {
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	killErr := cmd.Process.Kill()
	if killErr != nil {
		slog.Warn("storage: kill rclone serve", "error", killErr)
	}
	if waitErr := cmd.Wait(); waitErr != nil {
		// Expected: "signal: killed" — not an error from our perspective.
		slog.Debug("storage: rclone serve wait", "error", waitErr)
	}
	return killErr
}
