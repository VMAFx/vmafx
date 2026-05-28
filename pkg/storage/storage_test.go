// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/storage/storage_test.go — unit tests for the storage package.
//
// Tests run without a real rclone binary.  Subprocess-spawning behaviour is
// covered by TestHTTPServeStorage_LocalPassthrough and
// TestFUSEMountStorage_LocalPassthrough which exercise the local-path fast path.
// Integration tests (real rclone + local filesystem remote) require a live
// rclone binary and are skipped unless VMAFX_STORAGE_INTEGRATION_TEST=1 is set.
//
// ADR-0719: vmafx-node rclone integration.
package storage

import (
	"context"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// TestIsLocal checks URI classification.
func TestIsLocal(t *testing.T) {
	t.Parallel()
	cases := []struct {
		uri  string
		want bool
	}{
		{"/tmp/ref.yuv", true},
		{"./ref.yuv", true},
		{"file:///tmp/ref.yuv", true},
		{"file://localhost/tmp/ref.yuv", true},
		{"s3://bucket/ref.yuv", false},
		{"rclone://s3-prod:bucket/ref.yuv", false},
		{"gcs://bucket/ref.yuv", false},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.uri, func(t *testing.T) {
			t.Parallel()
			got := IsLocal(tc.uri)
			if got != tc.want {
				t.Errorf("IsLocal(%q) = %v, want %v", tc.uri, got, tc.want)
			}
		})
	}
}

// TestSplitRemotePath checks the root/asset splitting logic.
func TestSplitRemotePath(t *testing.T) {
	t.Parallel()
	cases := []struct {
		remotePath string
		wantRoot   string
		wantAsset  string
	}{
		{"s3:bucket/prefix/ref.yuv", "s3:bucket/prefix", "ref.yuv"},
		{"s3:bucket/ref.yuv", "s3:bucket", "ref.yuv"},
		{"gcs:bucket/dir/sub/dis.yuv", "gcs:bucket/dir/sub", "dis.yuv"},
		{"local:/data/ref.yuv", "local:/data", "ref.yuv"},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.remotePath, func(t *testing.T) {
			t.Parallel()
			root, asset := splitRemotePath(tc.remotePath)
			if root != tc.wantRoot || asset != tc.wantAsset {
				t.Errorf("splitRemotePath(%q) = (%q, %q), want (%q, %q)",
					tc.remotePath, root, asset, tc.wantRoot, tc.wantAsset)
			}
		})
	}
}

// TestRcloneRemotePath checks URI → rclone remote:path translation.
func TestRcloneRemotePath(t *testing.T) {
	t.Parallel()
	cases := []struct {
		uri  string
		want string
	}{
		{"s3://bucket/key/file.yuv", "s3:bucket/key/file.yuv"},
		{"gcs://mybucket/prefix/ref.yuv", "gcs:mybucket/prefix/ref.yuv"},
		{"rclone://s3-prod:bucket/ref.yuv", "s3-prod:bucket/ref.yuv"},
		{"sftp://user@host/path/file.yuv", "sftp:user@host/path/file.yuv"},
		// Already rclone syntax — pass through unchanged.
		{"myremote:bucket/ref.yuv", "myremote:bucket/ref.yuv"},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.uri, func(t *testing.T) {
			t.Parallel()
			got, err := rcloneRemotePath(tc.uri)
			if err != nil {
				t.Fatalf("rcloneRemotePath(%q) error: %v", tc.uri, err)
			}
			if got != tc.want {
				t.Errorf("rcloneRemotePath(%q) = %q, want %q", tc.uri, got, tc.want)
			}
		})
	}
}

// TestHTTPServeStorage_LocalPassthrough verifies that local paths bypass rclone.
func TestHTTPServeStorage_LocalPassthrough(t *testing.T) {
	t.Parallel()
	s := &HTTPServeStorage{rcloneBin: "rclone", log: slog.Default()}

	tmpFile, err := os.CreateTemp(t.TempDir(), "ref-*.yuv")
	if err != nil {
		t.Fatal(err)
	}
	if closeErr := tmpFile.Close(); closeErr != nil {
		t.Fatal(closeErr)
	}

	url, cleanup, err := s.Prepare(context.Background(), tmpFile.Name())
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	defer cleanup()

	if url != tmpFile.Name() {
		t.Errorf("expected local path passthrough, got %q", url)
	}
}

// TestFUSEMountStorage_LocalPassthrough verifies that local paths bypass rclone.
func TestFUSEMountStorage_LocalPassthrough(t *testing.T) {
	t.Parallel()
	s := &FUSEMountStorage{rcloneBin: "rclone", log: slog.Default()}

	tmpFile, err := os.CreateTemp(t.TempDir(), "ref-*.yuv")
	if err != nil {
		t.Fatal(err)
	}
	if closeErr := tmpFile.Close(); closeErr != nil {
		t.Fatal(closeErr)
	}

	gotPath, cleanup, err := s.Prepare(context.Background(), tmpFile.Name())
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	defer cleanup()

	if gotPath != tmpFile.Name() {
		t.Errorf("expected local path passthrough, got %q", gotPath)
	}
}

// TestLocalStorage_FileURI verifies file:// URI stripping.
func TestLocalStorage_FileURI(t *testing.T) {
	t.Parallel()
	s := &LocalStorage{}

	tmpFile, err := os.CreateTemp(t.TempDir(), "ref-*.yuv")
	if err != nil {
		t.Fatal(err)
	}
	if closeErr := tmpFile.Close(); closeErr != nil {
		t.Fatal(closeErr)
	}

	fileURI := "file://" + tmpFile.Name()
	got, cleanup, err := s.Prepare(context.Background(), fileURI)
	if err != nil {
		t.Fatalf("Prepare(%q): %v", fileURI, err)
	}
	defer cleanup()

	if got != tmpFile.Name() {
		t.Errorf("Prepare(%q) = %q, want %q", fileURI, got, tmpFile.Name())
	}
}

// TestAutoSelectMode verifies that New() returns HTTPServeStorage for auto mode.
func TestAutoSelectMode(t *testing.T) {
	t.Parallel()
	s := New(Config{Mode: ModeAuto, Log: slog.Default()})
	if s.Mode() != ModeHTTPServe {
		t.Errorf("auto mode should resolve to ModeHTTPServe, got %v", s.Mode())
	}
}

// TestMountModeSelect verifies that New() returns FUSEMountStorage for mount mode.
func TestMountModeSelect(t *testing.T) {
	t.Parallel()
	s := New(Config{Mode: ModeMount, Log: slog.Default()})
	if s.Mode() != ModeMount {
		t.Errorf("mount mode should return FUSEMountStorage, got %v", s.Mode())
	}
}

// TestWaitForHTTP verifies the readiness poller with a mock HTTP server.
func TestWaitForHTTP(t *testing.T) {
	t.Parallel()

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()

	// Strip the "http://" prefix to get just "host:port".
	addr := srv.Listener.Addr().String()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := waitForHTTP(ctx, addr, 5*time.Second); err != nil {
		t.Fatalf("waitForHTTP: %v", err)
	}
}

// TestWaitForPath verifies the filesystem readiness poller.
func TestWaitForPath(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	assetPath := filepath.Join(dir, "file.yuv")

	// Create the file after a short delay in a goroutine.
	go func() {
		time.Sleep(50 * time.Millisecond)
		f, err := os.Create(assetPath) //nolint:gosec -- test-only temp path
		if err != nil {
			return
		}
		if closeErr := f.Close(); closeErr != nil {
			return
		}
	}()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := waitForPath(ctx, assetPath, 5*time.Second); err != nil {
		t.Fatalf("waitForPath: %v", err)
	}
}

// TestPrepareCleanup_LocalDoesNotHang verifies cleanup on local paths is
// instantaneous and does not block.
func TestPrepareCleanup_LocalDoesNotHang(t *testing.T) {
	t.Parallel()
	s := New(Config{Mode: ModeHTTPServe, Log: slog.Default()})

	tmpFile, err := os.CreateTemp(t.TempDir(), "ref-*.yuv")
	if err != nil {
		t.Fatal(err)
	}
	if closeErr := tmpFile.Close(); closeErr != nil {
		t.Fatal(closeErr)
	}

	done := make(chan struct{})
	go func() {
		defer close(done)
		_, cleanup, err := s.Prepare(context.Background(), tmpFile.Name())
		if err != nil {
			t.Errorf("Prepare: %v", err)
			return
		}
		cleanup()
	}()

	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Error("cleanup blocked for > 3 s on a local path")
	}
}

// TestHTTPServeIntegration runs an end-to-end test with a real rclone binary
// and a local filesystem remote.  Skipped unless VMAFX_STORAGE_INTEGRATION_TEST=1.
func TestHTTPServeIntegration(t *testing.T) {
	if os.Getenv("VMAFX_STORAGE_INTEGRATION_TEST") != "1" {
		t.Skip("set VMAFX_STORAGE_INTEGRATION_TEST=1 to run rclone integration tests")
	}

	// Write a test YUV file.
	dir := t.TempDir()
	srcPath := filepath.Join(dir, "ref.yuv")
	if err := os.WriteFile(srcPath, []byte("fake yuv data"), 0o600); err != nil {
		t.Fatal(err)
	}

	// Build a minimal rclone config with a local remote pointing at dir.
	cfgPath := filepath.Join(dir, "rclone.conf")
	cfgContent := "[testlocal]\ntype = local\n"
	if err := os.WriteFile(cfgPath, []byte(cfgContent), 0o600); err != nil {
		t.Fatal(err)
	}

	s := &HTTPServeStorage{
		rcloneBin:    "rclone",
		rcloneConfig: cfgPath,
		log:          slog.Default(),
	}

	// Use the testlocal: remote.
	sourceURI := "testlocal:" + dir + "/ref.yuv"
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	url, cleanup, err := s.Prepare(ctx, sourceURI)
	if err != nil {
		t.Fatalf("Prepare(%q): %v", sourceURI, err)
	}
	defer cleanup()

	// Verify the asset is reachable via HTTP.
	resp, err := http.Get(url) //nolint:gosec,noctx -- integration test
	if err != nil {
		t.Fatalf("GET %q: %v", url, err)
	}
	defer func() {
		if closeErr := resp.Body.Close(); closeErr != nil {
			t.Logf("close response body: %v", closeErr)
		}
	}()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("GET %q status = %d, want 200", url, resp.StatusCode)
	}
}
