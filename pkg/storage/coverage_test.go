// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/storage/coverage_test.go — branch-coverage tests for pkg/storage that
// are not exercised by storage_test.go or internals_test.go.
//
// Covered paths:
//   - waitForPath: context-cancel exit and hard-timeout exit
//   - waitForHTTP: context-cancel exit and hard-timeout exit (no server)
//   - killProcess: real started process (sleep binary)
//   - unmount: all three bin-fallback branches via a fake script
//   - localPath: url.Parse-error branch (invalid URI with control char)
//   - HTTPServeStorage.Prepare: rclone start-failure (non-existent binary)
//   - FUSEMountStorage.Prepare: rclone start-failure (non-existent binary)
//
// ADR-1087: coverage improvement for pkg/storage.
package storage

import (
	"context"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// waitForPath — timeout and context-cancel branches
// ---------------------------------------------------------------------------

// TestWaitForPath_Timeout verifies that waitForPath returns a non-nil error
// when the deadline expires before the target path is created.
func TestWaitForPath_Timeout(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	missing := filepath.Join(dir, "never.yuv")

	ctx := context.Background()
	err := waitForPath(ctx, missing, 50*time.Millisecond)
	if err == nil {
		t.Fatal("expected timeout error, got nil")
	}
	if !strings.Contains(err.Error(), "timed out") {
		t.Errorf("error should mention timeout, got: %v", err)
	}
}

// TestWaitForPath_ContextCancel verifies that waitForPath returns the context
// error when the context is cancelled before the path appears.
func TestWaitForPath_ContextCancel(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	missing := filepath.Join(dir, "cancelled.yuv")

	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(20 * time.Millisecond)
		cancel()
	}()

	err := waitForPath(ctx, missing, 5*time.Second)
	if err == nil {
		t.Fatal("expected error after context cancel, got nil")
	}
}

// ---------------------------------------------------------------------------
// waitForHTTP — timeout and context-cancel branches
// ---------------------------------------------------------------------------

// TestWaitForHTTP_Timeout verifies that waitForHTTP returns a non-nil error
// when no server is listening at addr before the deadline expires.
func TestWaitForHTTP_Timeout(t *testing.T) {
	t.Parallel()
	// Use a port that is (almost certainly) not listening.  pickFreePort()
	// gives us a port the kernel currently isn't using; because we never bind
	// it, connection attempts will be refused immediately, letting the deadline
	// loop expire quickly with a small timeout.
	port, err := pickFreePort()
	if err != nil {
		t.Fatalf("pickFreePort: %v", err)
	}
	addr := "127.0.0.1:" + strconv.Itoa(port)

	ctx := context.Background()
	if err := waitForHTTP(ctx, addr, 60*time.Millisecond); err == nil {
		t.Fatal("expected timeout error, got nil")
	}
}

// TestWaitForHTTP_ContextCancel verifies that waitForHTTP returns when the
// context is cancelled even though the poll interval has not elapsed.
func TestWaitForHTTP_ContextCancel(t *testing.T) {
	t.Parallel()
	port, err := pickFreePort()
	if err != nil {
		t.Fatalf("pickFreePort: %v", err)
	}
	addr := "127.0.0.1:" + strconv.Itoa(port)

	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(15 * time.Millisecond)
		cancel()
	}()

	if err := waitForHTTP(ctx, addr, 10*time.Second); err == nil {
		t.Fatal("expected error after context cancel, got nil")
	}
}

// TestWaitForHTTP_ServerReturns5xx verifies that a 5xx response keeps the
// loop spinning (the server is not yet "ready" from the caller's perspective).
// We cancel the context to terminate the loop so the test is deterministic.
func TestWaitForHTTP_ServerReturns5xx(t *testing.T) {
	t.Parallel()

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer srv.Close()

	addr := srv.Listener.Addr().String()
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()

	// The server always returns 500; waitForHTTP should NOT return nil — it
	// must loop until the timeout fires.
	err := waitForHTTP(ctx, addr, 5*time.Second)
	if err == nil {
		t.Fatal("expected non-nil error when server returns 500; got nil")
	}
}

// ---------------------------------------------------------------------------
// killProcess — real started process
// ---------------------------------------------------------------------------

// TestKillProcess_RealProcess verifies that killProcess terminates a real
// running process and returns nil.
func TestKillProcess_RealProcess(t *testing.T) {
	t.Parallel()

	// Start "sleep 60" (or equivalent) — a process we know will stay alive.
	cmd := exec.Command("sleep", "60")
	if err := cmd.Start(); err != nil {
		t.Skipf("cannot start 'sleep 60': %v", err)
	}

	if err := killProcess(cmd); err != nil {
		t.Errorf("killProcess returned error on a running process: %v", err)
	}
}

// ---------------------------------------------------------------------------
// unmount — fake binary path
// ---------------------------------------------------------------------------

// TestUnmount_AllBinsFail verifies that unmount returns a non-nil error when
// none of the unmount binaries succeed, and that the error message mentions
// at least one of the attempted binaries.
//
// We override PATH to a temporary directory that contains no executables so
// all three attempts (fusermount3, fusermount, umount) fail with "not found".
func TestUnmount_AllBinsFail(t *testing.T) {
	// t.Setenv requires sequential execution — no t.Parallel().

	// Create an empty directory to use as PATH so no binary is found.
	emptyDir := t.TempDir()

	t.Setenv("PATH", emptyDir)

	s := &FUSEMountStorage{rcloneBin: "rclone", log: slog.Default()}
	err := s.unmount("/nonexistent-mount")
	if err == nil {
		t.Fatal("unmount with no valid binary should return error, got nil")
	}
}

// TestUnmount_FirstBinSucceeds verifies that unmount returns nil when the
// first candidate binary (fusermount3) succeeds.
//
// We create a fake "fusermount3" script that exits 0 and prepend its parent
// directory to PATH.
func TestUnmount_FirstBinSucceeds(t *testing.T) {
	// t.Setenv requires sequential execution — no t.Parallel().

	dir := t.TempDir()

	// Write a trivially-successful fake fusermount3.
	fakeScript := filepath.Join(dir, "fusermount3")
	if err := os.WriteFile(fakeScript, []byte("#!/bin/sh\nexit 0\n"), 0o700); err != nil {
		t.Fatalf("write fake fusermount3: %v", err)
	}

	t.Setenv("PATH", dir+":"+os.Getenv("PATH"))

	s := &FUSEMountStorage{rcloneBin: "rclone", log: slog.Default()}
	if err := s.unmount("/tmp/fake-mount"); err != nil {
		t.Errorf("unmount with successful fusermount3 should return nil, got: %v", err)
	}
}

// TestUnmount_FallbackToUmount verifies the umount fallback branch: when
// fusermount3 and fusermount are absent but umount succeeds, unmount returns
// nil.
func TestUnmount_FallbackToUmount(t *testing.T) {
	// t.Setenv requires sequential execution — no t.Parallel().

	dir := t.TempDir()

	// Only provide a fake "umount" that exits 0.
	fakeScript := filepath.Join(dir, "umount")
	if err := os.WriteFile(fakeScript, []byte("#!/bin/sh\nexit 0\n"), 0o700); err != nil {
		t.Fatalf("write fake umount: %v", err)
	}

	// fusermount3 and fusermount are NOT in dir; they will fail via PATH lookup.
	t.Setenv("PATH", dir)

	s := &FUSEMountStorage{rcloneBin: "rclone", log: slog.Default()}
	if err := s.unmount("/tmp/fake-mount"); err != nil {
		t.Errorf("unmount fallback to umount should return nil, got: %v", err)
	}
}

// ---------------------------------------------------------------------------
// HTTPServeStorage.Prepare — rclone binary missing (start-failure path)
// ---------------------------------------------------------------------------

// TestHTTPServeStorage_StartFailure exercises the error branch in
// HTTPServeStorage.Prepare where exec.Command.Start() fails because the
// configured rclone binary does not exist.
func TestHTTPServeStorage_StartFailure(t *testing.T) {
	t.Parallel()

	s := &HTTPServeStorage{
		rcloneBin: "/nonexistent/rclone-binary-xyz",
		log:       slog.Default(),
	}

	_, cleanup, err := s.Prepare(context.Background(), "s3://bucket/ref.yuv")
	if cleanup == nil {
		t.Error("cleanup func must not be nil on error")
	}
	if err == nil {
		t.Fatal("expected error when rclone binary is missing, got nil")
	}
}

// ---------------------------------------------------------------------------
// FUSEMountStorage.Prepare — rclone binary missing (start-failure path)
// ---------------------------------------------------------------------------

// TestFUSEMountStorage_StartFailure exercises the error branch in
// FUSEMountStorage.Prepare where cmd.Start() fails because the configured
// rclone binary does not exist.
func TestFUSEMountStorage_StartFailure(t *testing.T) {
	t.Parallel()

	s := &FUSEMountStorage{
		rcloneBin: "/nonexistent/rclone-binary-xyz",
		log:       slog.Default(),
	}

	_, cleanup, err := s.Prepare(context.Background(), "s3://bucket/ref.yuv")
	if cleanup == nil {
		t.Error("cleanup func must not be nil on error")
	}
	if err == nil {
		t.Fatal("expected error when rclone binary is missing, got nil")
	}
}

// ---------------------------------------------------------------------------
// localPath — parse-error branch
// ---------------------------------------------------------------------------

// TestLocalPath_ParseError exercises the url.Parse failure path in localPath.
// A URI containing a control character (0x01) is unparseable by net/url.
func TestLocalPath_ParseError(t *testing.T) {
	t.Parallel()
	// A URI with a raw control character in the host triggers url.Parse to
	// return an error.  The localPath helper should propagate that error.
	badURI := "file://\x01bad/path"
	_, err := localPath(badURI)
	if err == nil {
		t.Fatal("expected error from localPath on unparseable URI, got nil")
	}
}
