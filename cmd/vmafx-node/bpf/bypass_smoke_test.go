// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build linux

// bypass_smoke_test.go — smoke tests for the eBPF loader's compile-time
// surface and feature flag; no kernel privileges needed.
//
// The earlier read-latency benchmark (TestReadLatencyComparison) was removed:
// no read path uses the tracked descriptors (ADR-1539), so it measured two
// identical FUSE reads, and it skipped itself everywhere but a privileged host.
//
// ADR-0779.
package bpf

import (
	"log/slog"
	"testing"
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
