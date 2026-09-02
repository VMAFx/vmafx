// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/http_cancel_test.go — request-context cancellation tests.
//
// Verifies that an HTTP client disconnect mid-/v1/score-request causes the
// underlying vmaf subprocess to receive SIGKILL via exec.CommandContext, so
// the server does not leak background work after the client has gone away.
//
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.

//go:build cgo

package main

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// waitForPidFile blocks until the named file contains at least one byte or
// the deadline elapses.  Used by the cancel-after-start tests to make sure
// the sleeping subprocess has actually entered its sleep before we cancel.
func waitForPidFile(path string, deadline time.Duration) error {
	end := time.Now().Add(deadline)
	for time.Now().Before(end) {
		fi, err := os.Stat(path)
		if err == nil && fi.Size() > 0 {
			return nil
		}
		time.Sleep(10 * time.Millisecond)
	}
	return errors.New("pidfile never populated")
}

// pidIsAlive returns true if signal(0) succeeds against the parsed PID.
func pidIsAlive(pidStr string) bool {
	pid, err := strconv.Atoi(pidStr)
	if err != nil {
		return false
	}
	proc, err := os.FindProcess(pid)
	if err != nil {
		return false
	}
	return proc.Signal(syscall.Signal(0)) == nil
}

// writeSleepingVmafStubServer writes a vmaf stub that records its PID to a
// sentinel file then sleeps long enough for the test to cancel.  Mirrors the
// helper in pkg/libvmaf/libvmaf_test.go but lives in this package so the HTTP
// handler test can drive a real Scorer through a real listening server.
func writeSleepingVmafStubServer(t *testing.T) (scriptPath, pidFile string) {
	t.Helper()
	dir := t.TempDir()
	pidFile = filepath.Join(dir, "vmaf.pid")
	script := `#!/bin/sh
# Sleeping vmaf stub for cancel tests.  Writes our PID to ` + pidFile + `
# then sleeps; the handler-side context cancel propagates SIGKILL via
# exec.CommandContext.
echo $$ > ` + pidFile + `
sleep 30
exit 0
`
	scriptPath = filepath.Join(dir, "vmaf")
	if err := os.WriteFile(scriptPath, []byte(script), 0o700); err != nil {
		t.Fatalf("writeSleepingVmafStubServer: %v", err)
	}
	return scriptPath, pidFile
}

// TestScoreHandler_ClientDisconnectKillsSubprocess wires a real httpServer +
// real libvmaf.Scorer against a sleeping vmaf stub, fires a POST /v1/score,
// closes the client connection mid-request via context cancel, and asserts
// the subprocess receives SIGKILL within a short grace window.
//
// Without the ctx-aware Score signature the subprocess would outlive the
// request by ~30s; with it, exec.CommandContext + WaitDelay tears it down
// within ~2s.  Tolerate 5s here to absorb scheduler jitter on shared CI.
//
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
func TestScoreHandler_ClientDisconnectKillsSubprocess(t *testing.T) {
	stub, pidFile := writeSleepingVmafStubServer(t)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	hs := newHTTPServer(scorer, metrics, reg, log, nil)

	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Build a request with a cancellable context.  We DO NOT use a client
	// timeout because httptest.Client respects ctx-cancel via the request
	// body cancel signal — that is the exact mechanism a real client would
	// hit when it drops the TCP connection.
	reqCtx, cancel := context.WithCancel(context.Background())
	defer cancel()

	body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
	req, err := http.NewRequestWithContext(reqCtx, http.MethodPost,
		ts.URL+"/v1/score", strings.NewReader(body))
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	req.Header.Set("Content-Type", "application/json")

	var wg sync.WaitGroup
	var sawErr atomic.Bool
	wg.Add(1)
	go func() {
		defer wg.Done()
		resp, doErr := ts.Client().Do(req)
		if doErr != nil {
			sawErr.Store(true)
			return
		}
		// We don't expect a clean response — the handler should have its
		// Score call cancelled.  Either way drain the body so the test
		// doesn't leak fds.
		_ = resp.Body.Close()
	}()

	// Wait for the stub to write its PID, then cancel.
	if err := waitForPidFile(pidFile, 5*time.Second); err != nil {
		cancel()
		wg.Wait()
		t.Fatalf("stub never wrote PID file: %v", err)
	}
	cancel()
	wg.Wait()

	// Confirm Do() returned (cancellation propagated to the client side).
	if !sawErr.Load() {
		// Either the handler completed (impossible — vmaf stub sleeps 30s)
		// or it returned an error response (acceptable as long as the
		// process below is reaped).  Keep going to the kill assertion.
		t.Log("client side did not see a context-cancel error; continuing to kill assertion")
	}

	// Verify the subprocess PID is no longer alive within 5s.
	pidBytes, err := os.ReadFile(pidFile)
	if err != nil {
		t.Fatalf("read pidFile: %v", err)
	}
	pidStr := strings.TrimSpace(string(pidBytes))
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if !pidIsAlive(pidStr) {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	if pidIsAlive(pidStr) {
		t.Errorf("subprocess PID %s still alive 5s after client disconnect", pidStr)
	}
}
