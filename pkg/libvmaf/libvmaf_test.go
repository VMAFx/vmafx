// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/libvmaf_test.go — unit tests for the libvmaf Go wrapper.
//
// These tests exercise the JSON-parsing and model-resolution logic without
// requiring a live vmaf binary (mocked via a small shell script written to
// a temp dir).
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package libvmaf

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/pkg/model"
)

// writeVmafScript writes a minimal vmaf stub that emits a canned JSON
// response and returns its path.
func writeVmafScript(t *testing.T, jsonPayload string) string {
	t.Helper()
	dir := t.TempDir()

	// Write the canned JSON output file when the stub receives -o <path>.
	script := `#!/bin/sh
# Minimal vmaf stub: parse -o flag and write canned JSON.
outfile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) outfile="$2"; shift 2 ;;
    *)  shift ;;
  esac
done
if [ -n "$outfile" ]; then
  cat > "$outfile" <<'EOF'
` + jsonPayload + `
EOF
fi
exit 0
`
	scriptPath := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(scriptPath, []byte(script), 0o700); err != nil {
		t.Fatalf("writeVmafScript: %v", err)
	}
	return scriptPath
}

// writeModel writes a placeholder .json model file to dir and returns its path.
func writeModel(t *testing.T, dir, name string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("writeModel MkdirAll: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, name+".json"), []byte(`{}`), 0o600); err != nil {
		t.Fatalf("writeModel WriteFile: %v", err)
	}
}

// goldenJSON is a representative vmaf CLI JSON output payload.
const goldenJSON = `{
  "pooled_metrics": {
    "vmaf":          {"mean": 76.6683, "harmonic_mean": 76.1234},
    "vif_scale0":    {"mean": 0.8912},
    "vif_scale1":    {"mean": 0.9301},
    "motion2":       {"mean": 2.3456},
    "adm2":          {"mean": 0.9876}
  }
}`

func TestNew_BinaryNotFound(t *testing.T) {
	_, err := New("/nonexistent/vmaf", "")
	if err == nil {
		t.Fatal("expected error for nonexistent binary, got nil")
	}
}

func TestNew_UsesPath(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, "")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if s == nil {
		t.Fatal("expected non-nil Scorer")
	}
}

func TestScore_ParsesGoldenJSON(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")

	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	score, features, err := s.Score(context.Background(), "ref.yuv", "dis.yuv", "vmaf_v0.6.1")
	if err != nil {
		t.Fatalf("Score: %v", err)
	}

	const wantScore = 76.6683
	if score != wantScore {
		t.Errorf("score: got %v, want %v", score, wantScore)
	}
	if len(features) == 0 {
		t.Error("expected non-empty features map")
	}
	if _, ok := features["vmaf"]; !ok {
		t.Error("features map missing 'vmaf' key")
	}
}

func TestScore_DefaultModel(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	modelDir := t.TempDir()
	// Stage the model the resolver will actually ask for. Naming it through
	// model.DefaultVersion rather than a literal keeps this fixture correct when
	// the default moves again — hardcoding "vmaf_v0.6.1" here is exactly what
	// broke when the default became vmaf_v1.0.16_3d0h (ADR-1169).
	writeModel(t, modelDir, model.DefaultVersion)

	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	// Empty model name → falls back to the built-in default.
	score, _, err := s.Score(context.Background(), "ref.yuv", "dis.yuv", "")
	if err != nil {
		t.Fatalf("Score with default model: %v", err)
	}
	if score != 76.6683 {
		t.Errorf("expected 76.6683, got %v", score)
	}
}

func TestScore_PassesModelAsCLIPathParameter(t *testing.T) {
	argsFile := filepath.Join(t.TempDir(), "vmaf-args.txt")
	t.Setenv("VMAF_ARGS_FILE", argsFile)

	dir := t.TempDir()
	scriptPath := filepath.Join(dir, "vmaf")
	script := `#!/bin/sh
printf '%s\n' "$@" > "$VMAF_ARGS_FILE"
outfile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) outfile="$2"; shift 2 ;;
    *)  shift ;;
  esac
done
cat > "$outfile" <<'EOF'
` + goldenJSON + `
EOF
`
	if err := os.WriteFile(scriptPath, []byte(script), 0o700); err != nil {
		t.Fatalf("write vmaf argument recorder: %v", err)
	}

	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")
	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if _, _, err := s.Score(context.Background(), "ref.y4m", "dis.y4m", "vmaf_v0.6.1"); err != nil {
		t.Fatalf("Score: %v", err)
	}

	argsBytes, err := os.ReadFile(argsFile)
	if err != nil {
		t.Fatalf("read recorded arguments: %v", err)
	}
	args := strings.Split(strings.TrimSuffix(string(argsBytes), "\n"), "\n")
	want := "path=" + filepath.Join(modelDir, "vmaf_v0.6.1.json")
	for i, arg := range args {
		if arg == "-m" && i+1 < len(args) {
			if args[i+1] != want {
				t.Fatalf("model argument: got %q, want %q", args[i+1], want)
			}
			return
		}
	}
	t.Fatalf("recorded arguments contain no -m value: %q", args)
}

func TestScore_ModelNotFound(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, t.TempDir())
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	_, _, err = s.Score(context.Background(), "ref.yuv", "dis.yuv", "nonexistent_model")
	if err == nil {
		t.Fatal("expected error for missing model, got nil")
	}
}

func TestParseOutput_MissingVmafKey(t *testing.T) {
	tmp, err := os.CreateTemp("", "vmafx-test-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())

	// JSON without a "vmaf" key in pooled_metrics.
	data := map[string]any{
		"pooled_metrics": map[string]any{
			"vif_scale0": map[string]any{"mean": 0.89},
		},
	}
	b, _ := json.Marshal(data)
	if err := os.WriteFile(tmp.Name(), b, 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	tmp.Close()

	_, _, err = parseOutput(tmp.Name())
	if err == nil {
		t.Fatal("expected error for missing vmaf key, got nil")
	}
}

func TestClose_IsNoOp(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, "")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	// Must not panic.
	s.Close()
}

// writeSleepingVmafScript writes a vmaf stub that records its PID to pidFile
// and then sleeps long enough for the test to cancel the context.  The script
// exits non-zero on SIGKILL/SIGTERM (POSIX-default).  We use a sentinel file
// rather than scanning `ps` so the test stays deterministic across hosts
// (some CI runners restrict /proc access).
func writeSleepingVmafScript(t *testing.T, pidFile string) string {
	t.Helper()
	dir := t.TempDir()
	script := `#!/bin/sh
# Sleeping vmaf stub for cancel tests.  Writes our PID to ` + pidFile + `
# then sleeps; exec.CommandContext cancellation hits us with SIGKILL.
echo $$ > ` + pidFile + `
sleep 30
exit 0
`
	scriptPath := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(scriptPath, []byte(script), 0o700); err != nil {
		t.Fatalf("writeSleepingVmafScript: %v", err)
	}
	return scriptPath
}

// TestScore_CancelKillsSubprocess verifies that cancelling the context passed
// to Score tears down the vmaf subprocess.  The harness:
//  1. Starts Score against a stub that sleeps for 30s and records its PID.
//  2. Cancels the context after 100ms.
//  3. Asserts Score returns within a short timeout (proves exec.CommandContext
//     fired SIGKILL — without ctx-aware exec, the call would block ~30s).
//  4. Confirms the recorded PID is no longer alive (signal 0 probe).
//
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
func TestScore_CancelKillsSubprocess(t *testing.T) {
	dir := t.TempDir()
	pidFile := filepath.Join(dir, "vmaf.pid")
	scriptPath := writeSleepingVmafScript(t, pidFile)
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")

	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())

	// Cancel after 100ms — by then the stub has written its PID and is
	// blocking inside sleep.
	go func() {
		time.Sleep(100 * time.Millisecond)
		cancel()
	}()

	// Run Score on a separate goroutine so we can enforce an outer wall-clock
	// timeout: without exec.CommandContext, Score would block ~30s and the
	// test would time out at the `go test` level, not at the assertion.
	done := make(chan error, 1)
	go func() {
		_, _, err := s.Score(ctx, "ref.yuv", "dis.yuv", "vmaf_v0.6.1")
		done <- err
	}()

	select {
	case err := <-done:
		if err == nil {
			t.Fatal("expected Score to return an error after cancel, got nil")
		}
		// We accept either the wrapped ctx.Err() or the underlying signal
		// error — both prove cancellation fired.  We DO NOT accept a clean
		// nil return because that would mean we waited the full sleep.
		if !errors.Is(err, context.Canceled) && !strings.Contains(err.Error(), "signal") &&
			!strings.Contains(err.Error(), "cancelled") && !strings.Contains(err.Error(), "killed") {
			t.Errorf("Score error did not signal cancellation: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Score did not return within 5s after cancel — subprocess outlived its context")
	}

	// Verify the PID is no longer alive.  signal 0 returns ESRCH if the
	// process no longer exists.  Tolerate a brief grace window for SIGKILL
	// to be delivered.
	pidBytes, err := os.ReadFile(pidFile)
	if err != nil {
		t.Fatalf("read pidFile: %v", err)
	}
	pidStr := strings.TrimSpace(string(pidBytes))
	if pidStr == "" {
		t.Fatal("stub never wrote a PID — script may not have executed")
	}
	// Wait up to 2s for the kernel to reap the process.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if err := pidAlive(pidStr); err != nil {
			// Process gone — success.
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	if err := pidAlive(pidStr); err == nil {
		t.Errorf("subprocess PID %s still alive 2s after cancel", pidStr)
	}
}

// pidAlive returns nil when the process named by pidStr is alive and
// receivable to signal 0, or a non-nil error otherwise (typically ESRCH).
func pidAlive(pidStr string) error {
	pid, err := strconv.Atoi(pidStr)
	if err != nil {
		return err
	}
	proc, err := os.FindProcess(pid)
	if err != nil {
		return err
	}
	return proc.Signal(syscall.Signal(0))
}

// TestScore_PreCancelledContext verifies that a context that is already
// cancelled before Score is called returns immediately without forking a
// subprocess.  This documents the fast-path behaviour and protects against
// regressions that would happily fire and then race the cancel signal.
func TestScore_PreCancelledContext(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")
	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // pre-cancel before any work

	start := time.Now()
	_, _, err = s.Score(ctx, "ref.yuv", "dis.yuv", "vmaf_v0.6.1")
	elapsed := time.Since(start)
	if err == nil {
		t.Fatal("expected error from pre-cancelled context, got nil")
	}
	if !errors.Is(err, context.Canceled) {
		t.Errorf("expected context.Canceled, got %v", err)
	}
	if elapsed > 500*time.Millisecond {
		t.Errorf("pre-cancelled Score took %v — fast-path not working", elapsed)
	}
}
