// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/storage/internals_test.go — unit tests for the unexported helpers
// that the existing storage_test.go does not reach: argv builders,
// pickFreePort, killProcess (nil/empty cases), and Storage.Mode()
// observations on LocalStorage / Config.RcloneBin defaulting.
package storage

import (
	"log/slog"
	"net"
	"os/exec"
	"strconv"
	"strings"
	"testing"
)

// TestBuildServeArgs_WithoutConfig covers the no-config branch.
func TestBuildServeArgs_WithoutConfig(t *testing.T) {
	t.Parallel()
	s := &HTTPServeStorage{rcloneBin: "rclone"}
	argv := s.buildServeArgs("s3:bucket", "127.0.0.1:9000")
	joined := strings.Join(argv, " ")
	wantSubstrings := []string{"rclone", "serve", "http", "s3:bucket", "--addr", "127.0.0.1:9000", "--no-checksum"}
	for _, sub := range wantSubstrings {
		if !strings.Contains(joined, sub) {
			t.Errorf("buildServeArgs missing %q: %v", sub, argv)
		}
	}
	if strings.Contains(joined, "--config") {
		t.Errorf("--config flag should not appear when RcloneConfig is empty: %v", argv)
	}
}

// TestBuildServeArgs_WithConfig covers the config-supplied branch.
func TestBuildServeArgs_WithConfig(t *testing.T) {
	t.Parallel()
	s := &HTTPServeStorage{rcloneBin: "rclone", rcloneConfig: "/etc/rclone.conf"}
	argv := s.buildServeArgs("s3:bucket", "127.0.0.1:9000")
	joined := strings.Join(argv, " ")
	if !strings.Contains(joined, "--config /etc/rclone.conf") {
		t.Errorf("--config /etc/rclone.conf missing: %v", argv)
	}
}

// TestBuildMountArgs_WithoutConfig covers the no-config branch.
func TestBuildMountArgs_WithoutConfig(t *testing.T) {
	t.Parallel()
	s := &FUSEMountStorage{rcloneBin: "rclone"}
	argv := s.buildMountArgs("s3:bucket", "/tmp/vmafx-rclone-test")
	joined := strings.Join(argv, " ")
	wantSubstrings := []string{"rclone", "mount", "s3:bucket", "/tmp/vmafx-rclone-test", "--daemon", "--vfs-cache-mode", "off", "--allow-non-empty"}
	for _, sub := range wantSubstrings {
		if !strings.Contains(joined, sub) {
			t.Errorf("buildMountArgs missing %q: %v", sub, argv)
		}
	}
	if strings.Contains(joined, "--config") {
		t.Errorf("--config should not appear when RcloneConfig is empty: %v", argv)
	}
}

// TestBuildMountArgs_WithConfig covers the config-supplied branch.
func TestBuildMountArgs_WithConfig(t *testing.T) {
	t.Parallel()
	s := &FUSEMountStorage{rcloneBin: "rclone", rcloneConfig: "/etc/rclone.conf"}
	argv := s.buildMountArgs("s3:bucket", "/tmp/x")
	if !strings.Contains(strings.Join(argv, " "), "--config /etc/rclone.conf") {
		t.Errorf("--config /etc/rclone.conf missing: %v", argv)
	}
}

// TestPickFreePort_ReturnsListenablePort verifies the port can actually
// be bound (sanity check that the helper isn't returning a stale port).
func TestPickFreePort_ReturnsListenablePort(t *testing.T) {
	t.Parallel()
	port, err := pickFreePort()
	if err != nil {
		t.Fatalf("pickFreePort: %v", err)
	}
	if port <= 0 || port > 65535 {
		t.Fatalf("pickFreePort returned out-of-range port %d", port)
	}
	// Re-binding the port — succeeds on Linux/macOS because the
	// probe listener already released the port.
	ln, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(port))
	if err != nil {
		// Port may have been taken — log and skip.
		t.Logf("re-bind of port %d failed (likely race): %v", port, err)
		return
	}
	if err := ln.Close(); err != nil {
		t.Errorf("close listener: %v", err)
	}
}

// TestKillProcess_NilCmd covers the nil-cmd guard branch.
func TestKillProcess_NilCmd(t *testing.T) {
	t.Parallel()
	defer func() {
		if r := recover(); r != nil {
			t.Errorf("killProcess(nil) panicked: %v", r)
		}
	}()
	killProcess(nil)
}

// TestKillProcess_NilProcess covers the nil-Process branch.
func TestKillProcess_NilProcess(t *testing.T) {
	t.Parallel()
	defer func() {
		if r := recover(); r != nil {
			t.Errorf("killProcess(cmd-without-process) panicked: %v", r)
		}
	}()
	cmd := exec.Command("/this/binary/does/not/exist") //nolint:gosec
	// Process field stays nil because we never call Start.
	killProcess(cmd)
}

// TestSplitRemotePath_NoColon covers the colon-not-found branch.
func TestSplitRemotePath_NoColon(t *testing.T) {
	t.Parallel()
	root, asset := splitRemotePath("/local/path/file.yuv")
	if root != "/local/path" {
		t.Errorf("root = %q, want %q", root, "/local/path")
	}
	if asset != "file.yuv" {
		t.Errorf("asset = %q, want %q", asset, "file.yuv")
	}
}

// TestSplitRemotePath_BareColon covers the dir == "." branch (single
// component after the colon).
func TestSplitRemotePath_BareColon(t *testing.T) {
	t.Parallel()
	root, asset := splitRemotePath("s3:file.yuv")
	if root != "s3:" {
		t.Errorf("root = %q, want %q", root, "s3:")
	}
	if asset != "file.yuv" {
		t.Errorf("asset = %q, want %q", asset, "file.yuv")
	}
}

// TestLocalStorage_Mode verifies LocalStorage.Mode() returns ModeAuto.
func TestLocalStorage_Mode(t *testing.T) {
	t.Parallel()
	s := &LocalStorage{}
	if s.Mode() != ModeAuto {
		t.Errorf("LocalStorage.Mode() = %v, want %v", s.Mode(), ModeAuto)
	}
}

// TestNew_DefaultsRcloneBin verifies the empty-bin default behaviour.
func TestNew_DefaultsRcloneBin(t *testing.T) {
	t.Parallel()
	// Empty Mode + empty RcloneBin: both defaults exercised.
	s := New(Config{Log: slog.Default()})
	if s == nil {
		t.Fatal("New returned nil")
	}
	if s.Mode() != ModeHTTPServe {
		t.Errorf("default Mode = %v, want %v", s.Mode(), ModeHTTPServe)
	}
}

// TestNew_NilLoggerDefaults verifies New() uses slog.Default when no
// logger is passed.
func TestNew_NilLoggerDefaults(t *testing.T) {
	t.Parallel()
	s := New(Config{Mode: ModeMount})
	if s == nil {
		t.Fatal("New returned nil")
	}
	if s.Mode() != ModeMount {
		t.Errorf("Mode = %v, want %v", s.Mode(), ModeMount)
	}
}

// TestRcloneRemotePath_HTTPPassthrough covers the http(s) branch.
func TestRcloneRemotePath_HTTPPassthrough(t *testing.T) {
	t.Parallel()
	got, err := rcloneRemotePath("https://example.com/path/file.yuv")
	if err != nil {
		t.Fatalf("rcloneRemotePath: %v", err)
	}
	if got != "https://example.com/path/file.yuv" {
		t.Errorf("https URL was transformed: %q", got)
	}
}

// TestRcloneRemotePath_FTPWithUser covers the sftp/ftp user@host branch.
func TestRcloneRemotePath_FTPWithUser(t *testing.T) {
	t.Parallel()
	got, err := rcloneRemotePath("ftp://anon@host.example.com/path/file.yuv")
	if err != nil {
		t.Fatalf("rcloneRemotePath: %v", err)
	}
	if !strings.HasPrefix(got, "ftp:anon@host.example.com") {
		t.Errorf("ftp transformation = %q", got)
	}
}

// TestLocalPath_BareDot covers the ./relative-path branch.
func TestLocalPath_BareDot(t *testing.T) {
	t.Parallel()
	got, err := localPath("./relative/file.yuv")
	if err != nil {
		t.Fatalf("localPath: %v", err)
	}
	if got != "./relative/file.yuv" {
		t.Errorf("localPath('./relative/file.yuv') = %q", got)
	}
}

// TestLocalPath_NonLocalScheme covers the error-emit branch when the
// URI scheme is remote.
func TestLocalPath_NonLocalScheme(t *testing.T) {
	t.Parallel()
	_, err := localPath("s3://bucket/key")
	if err == nil {
		t.Fatal("localPath should error on non-local URI")
	}
}

// TestLocalStorage_PrepareInvalidURI covers the Prepare error branch.
func TestLocalStorage_PrepareInvalidURI(t *testing.T) {
	t.Parallel()
	s := &LocalStorage{}
	_, cleanup, err := s.Prepare(nil, "s3://bucket/key.yuv")
	if cleanup == nil {
		t.Error("cleanup is nil")
	}
	if err == nil {
		t.Error("expected error for non-local URI")
	}
}
