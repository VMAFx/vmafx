// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// coverage_test.go — branch-coverage tests for the bpf package not exercised
// by bypass_unit_test.go or bypass_smoke_test.go.
//
// link.Link has an unexported isLink() method so external types cannot satisfy
// it; tests that require a real link.Link value need kernel CAP_BPF and are
// relegated to the integration path.  The tests here cover pure-Go paths:
//
//   - New() prefix normalisation (already-slash / needs-slash)
//   - rcloneBypassPrograms.Close / rcloneBypassMaps.Close with stub nil guards
//     already exercised by TestStubClose_Objects; this file adds documentation
//     that we intentionally skip the non-nil branches (need real ebpf.Program)
//
// ADR-1087: coverage improvement for pkg/storage (bpf sub-package).
package bpf

import (
	"testing"
)

// ---------------------------------------------------------------------------
// New() — prefix normalisation branches
// ---------------------------------------------------------------------------

// TestNew_PrefixAlreadyHasTrailingSlash verifies that a prefix that already
// ends in "/" is not double-slashed.
func TestNew_PrefixAlreadyHasTrailingSlash(t *testing.T) {
	t.Parallel()
	l := New("/mnt/rclone/", nil)
	if l.mountPrefix != "/mnt/rclone/" {
		t.Errorf("mountPrefix = %q, want %q", l.mountPrefix, "/mnt/rclone/")
	}
}

// TestNew_PrefixWithoutTrailingSlash verifies that New() appends "/" to a
// prefix that does not already end with one.
func TestNew_PrefixWithoutTrailingSlash(t *testing.T) {
	t.Parallel()
	l := New("/mnt/rclone", nil)
	if l.mountPrefix != "/mnt/rclone/" {
		t.Errorf("mountPrefix = %q, want %q", l.mountPrefix, "/mnt/rclone/")
	}
}

// TestNew_EmptyPrefixUsesDefault verifies that an empty mount prefix string
// results in DefaultMountPrefix being used.
func TestNew_EmptyPrefixUsesDefault(t *testing.T) {
	t.Parallel()
	l := New("", nil)
	if l.mountPrefix != DefaultMountPrefix {
		t.Errorf("mountPrefix = %q, want %q", l.mountPrefix, DefaultMountPrefix)
	}
}

// ---------------------------------------------------------------------------
// IsBypassFD — FdKey arithmetic boundary
// ---------------------------------------------------------------------------

// TestIsBypassFD_ZeroPidZeroFd verifies that (pid=0, fd=0) → fdKey=0 is a
// valid key and can be stored / retrieved correctly.
func TestIsBypassFD_ZeroPidZeroFd(t *testing.T) {
	t.Parallel()
	l := New("", nil)

	// Store under fdKey 0.
	l.mu.Lock()
	l.activeFDs[0] = "/zero/path"
	l.mu.Unlock()

	path, ok := l.IsBypassFD(0, 0)
	if !ok {
		t.Fatal("IsBypassFD(0,0): expected hit, got miss")
	}
	if path != "/zero/path" {
		t.Errorf("IsBypassFD(0,0): path = %q, want %q", path, "/zero/path")
	}
}

// TestIsBypassFD_MaxFd verifies that fd=0xFFFFFFFF is correctly encoded in
// the lower 32 bits of fdKey without sign-extension.
func TestIsBypassFD_MaxFd(t *testing.T) {
	t.Parallel()
	l := New("", nil)
	const pid = uint32(1)
	const fd = uint32(0xFFFFFFFF)
	fdKey := (uint64(pid) << 32) | uint64(fd)

	l.mu.Lock()
	l.activeFDs[fdKey] = "/max/fd"
	l.mu.Unlock()

	path, ok := l.IsBypassFD(pid, fd)
	if !ok {
		t.Fatal("IsBypassFD with max fd: expected hit, got miss")
	}
	if path != "/max/fd" {
		t.Errorf("IsBypassFD with max fd: path = %q, want %q", path, "/max/fd")
	}
}
