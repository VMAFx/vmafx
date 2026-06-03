// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/http_cancel_test.go — request-context cancellation tests.
//
// Verifies that an HTTP client disconnect mid-/v1/score-request causes the
// underlying vmaf subprocess to receive SIGKILL via exec.CommandContext, so
// the controller does not leak background work after the client has gone away.
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
// the deadline elapses.
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

// writeSleepingVmafStubController writes a vmaf stub that records its PID
// then sleeps long enough for the test to cancel.
func writeSleepingVmafStubController(t *testing.T) (scriptPath, pidFile string) {
	t.Helper()
	dir := t.TempDir()
	pidFile = filepath.Join(dir, "vmaf.pid")
	script := `#!/bin/sh
echo $$ > ` + pidFile + `
sleep 30
exit 0
`
	scriptPath = filepath.Join(dir, "vmaf")
	if err := os.WriteFile(scriptPath, []byte(script), 0o700); err != nil {
		t.Fatalf("writeSleepingVmafStubController: %v", err)
	}
	return scriptPath, pidFile
}

// TestScoreHandler_ClientDisconnectKillsSubprocess wires a real httpServer +
// real libvmaf.Scorer against a sleeping vmaf stub, fires a POST /v1/score,
// cancels the request context mid-flight, and asserts the subprocess receives
// SIGKILL within a short grace window.
//
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
func TestScoreHandler_ClientDisconnectKillsSubprocess(t *testing.T) {
	stub, pidFile := writeSleepingVmafStubController(t)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	hs := newHTTPServer(scorer, metrics, reg, nil, log)

	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

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
		_ = resp.Body.Close()
	}()

	if err := waitForPidFile(pidFile, 5*time.Second); err != nil {
		cancel()
		wg.Wait()
		t.Fatalf("stub never wrote PID file: %v", err)
	}
	cancel()
	wg.Wait()

	if !sawErr.Load() {
		t.Log("client side did not see a context-cancel error; continuing to kill assertion")
	}

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
