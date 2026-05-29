// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// bypass_smoke_test.go — smoke tests for the eBPF FUSE bypass loader.
//
// Tests that run with VMAFX_EBPF_BYPASS=0 (the default) exercise only the
// compile-time surface and feature-flag logic — no kernel privileges needed.
//
// The latency benchmark (TestReadLatencyComparison) requires:
//   - VMAFX_EBPF_BYPASS=1
//   - VMAFX_EBPF_SMOKE_RCLONE_MOUNT set to an accessible rclone-serve HTTP
//     mount path (default /tmp/rclone-smoke)
//   - CAP_BPF or CAP_SYS_ADMIN
//   - Linux 5.15+
//
// Run the full benchmark with:
//
//	VMAFX_EBPF_BYPASS=1 VMAFX_EBPF_SMOKE_RCLONE_MOUNT=/rclone-mount \
//	  go test -v -run TestReadLatencyComparison -timeout 120s \
//	  ./cmd/vmafx-node/bpf/
//
// ADR-0779.
package bpf

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// TestEnabledFlag verifies the VMAFX_EBPF_BYPASS env var is parsed correctly.
func TestEnabledFlag(t *testing.T) {
	t.Setenv("VMAFX_EBPF_BYPASS", "0")
	if Enabled() {
		t.Error("expected Enabled()=false when VMAFX_EBPF_BYPASS=0")
	}

	t.Setenv("VMAFX_EBPF_BYPASS", "1")
	if !Enabled() {
		t.Error("expected Enabled()=true when VMAFX_EBPF_BYPASS=1")
	}
}

// TestLoaderNew verifies New() accepts valid and empty prefixes without panicking.
func TestLoaderNew(t *testing.T) {
	l := New("", nil)
	if l == nil {
		t.Fatal("New returned nil")
	}
	if l.mountPrefix != DefaultMountPrefix {
		t.Errorf("expected default prefix %q, got %q", DefaultMountPrefix, l.mountPrefix)
	}

	l2 := New("/custom-mount", slog.Default())
	if l2.mountPrefix != "/custom-mount/" {
		t.Errorf("expected trailing slash, got %q", l2.mountPrefix)
	}
}

// TestIsBypassFDEmptyMap verifies IsBypassFD returns false on an empty cache.
func TestIsBypassFDEmptyMap(t *testing.T) {
	l := New("", nil)
	path, ok := l.IsBypassFD(12345, 3)
	if ok {
		t.Errorf("expected ok=false on empty cache, got path=%q", path)
	}
}

// TestStartFailsWithoutPrivilege verifies that Start returns a descriptive error
// when the real BPF objects cannot be loaded (stub always fails).
func TestStartFailsWithoutPrivilege(t *testing.T) {
	l := New("", slog.Default())
	err := l.Start(context.Background())
	if err == nil {
		t.Fatal("expected error from stub loader, got nil")
	}
	t.Logf("expected stub error: %v", err)
}

// TestReadLatencyComparison measures p50 read latency with and without the eBPF
// bypass for a 100 MiB sequential read from a rclone-mounted path.
//
// This test is a manual benchmark, not part of the CI fast suite.
// Skip silently unless VMAFX_EBPF_BYPASS=1 is explicitly set.
func TestReadLatencyComparison(t *testing.T) {
	if !Enabled() {
		t.Skip("VMAFX_EBPF_BYPASS not set to 1 — skipping live benchmark")
	}

	mountPath := os.Getenv("VMAFX_EBPF_SMOKE_RCLONE_MOUNT")
	if mountPath == "" {
		mountPath = "/tmp/rclone-smoke"
	}

	testFile := filepath.Join(mountPath, "smoke_100mb.bin")
	const targetBytes = 100 * 1024 * 1024 // 100 MiB

	// Ensure the test file exists (create synthetic file in mount dir if needed).
	if _, err := os.Stat(testFile); errors.Is(err, os.ErrNotExist) {
		t.Logf("test file %s not found — creating synthetic file under /tmp instead", testFile)
		testFile = filepath.Join(t.TempDir(), "smoke_100mb.bin")
		if err := createSyntheticFile(testFile, targetBytes); err != nil {
			t.Fatalf("create synthetic file: %v", err)
		}
		t.Logf("using synthetic file at %s (no FUSE layer — bypass won't activate)", testFile)
	}

	// Baseline: read without bypass.
	baselineP50, err := measureReadP50(testFile, targetBytes, 5)
	if err != nil {
		t.Fatalf("baseline read: %v", err)
	}
	t.Logf("baseline p50 read latency (no bypass): %v", baselineP50)

	// Start the eBPF loader.
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	loader := New(mountPath, slog.Default())
	if err := loader.Start(ctx); err != nil {
		// In environments without CAP_BPF the stub will fail here.
		t.Logf("eBPF loader start failed (expected without CAP_BPF): %v", err)
		t.Skip("eBPF loader unavailable — skipping bypass timing")
	}
	defer loader.Stop()

	// Warm-up: open the file through the mount so the eBPF probe registers it.
	f, err := os.Open(testFile)
	if err != nil {
		t.Fatalf("open test file: %v", err)
	}
	_ = f.Close()

	// Give the ring-buffer drain goroutine a moment to catch the event.
	time.Sleep(50 * time.Millisecond)

	// Read with bypass active.
	bypassP50, err := measureReadP50(testFile, targetBytes, 5)
	if err != nil {
		t.Fatalf("bypass read: %v", err)
	}
	t.Logf("bypass p50 read latency: %v", bypassP50)

	if bypassP50 > 0 && baselineP50 > 0 {
		ratio := float64(baselineP50) / float64(bypassP50)
		t.Logf("speedup ratio (baseline/bypass): %.1fx", ratio)
	}
}

// measureReadP50 opens the file, reads targetBytes n times, and returns the
// median (p50) wall-clock duration for a single full read.
func measureReadP50(path string, targetBytes int, n int) (time.Duration, error) {
	buf := make([]byte, 64*1024) // 64 KiB read buffer
	times := make([]time.Duration, 0, n)

	for i := 0; i < n; i++ {
		f, err := os.Open(path)
		if err != nil {
			return 0, fmt.Errorf("open: %w", err)
		}

		start := time.Now()
		total := 0
		for total < targetBytes {
			nr, err := f.Read(buf)
			total += nr
			if err == io.EOF {
				break
			}
			if err != nil {
				_ = f.Close()
				return 0, fmt.Errorf("read at offset %d: %w", total, err)
			}
		}
		elapsed := time.Since(start)
		_ = f.Close()
		times = append(times, elapsed)
	}

	// Sort and return median.
	for i := 1; i < len(times); i++ {
		for j := i; j > 0 && times[j] < times[j-1]; j-- {
			times[j], times[j-1] = times[j-1], times[j]
		}
	}
	return times[len(times)/2], nil
}

// createSyntheticFile writes size bytes of zero-filled data to path.
func createSyntheticFile(path string, size int) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()

	chunk := make([]byte, 64*1024)
	written := 0
	for written < size {
		n := size - written
		if n > len(chunk) {
			n = len(chunk)
		}
		nw, err := f.Write(chunk[:n])
		written += nw
		if err != nil {
			return err
		}
	}
	return nil
}
