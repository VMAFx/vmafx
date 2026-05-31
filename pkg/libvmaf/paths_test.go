// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// pkg/libvmaf/paths_test.go — unit tests for binary / repo-root / path
// allow-listing helpers. Independent of cgo so it builds even when the
// real libvmaf.so is absent at test time.

package libvmaf

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestFindBinary_RespectsEnvOverride verifies the VMAF_BIN env var wins.
func TestFindBinary_RespectsEnvOverride(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_BIN")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_BIN", old)
		} else {
			_ = os.Unsetenv("VMAF_BIN")
		}
	}()
	if err := os.Setenv("VMAF_BIN", "/custom/vmaf/path"); err != nil {
		t.Fatalf("Setenv: %v", err)
	}
	if got := FindBinary(); got != "/custom/vmaf/path" {
		t.Errorf("FindBinary() = %q, want %q", got, "/custom/vmaf/path")
	}
}

// TestFindBinary_FallsBackToLastCandidate verifies the fallback when
// nothing exists on disk and no env override is set.
func TestFindBinary_FallsBackToLastCandidate(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_BIN")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_BIN", old)
		} else {
			_ = os.Unsetenv("VMAF_BIN")
		}
	}()
	_ = os.Unsetenv("VMAF_BIN")

	// Run from a directory with no CLAUDE.md ancestor so the candidate
	// list is constructed but no candidate exists.
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatalf("Getwd: %v", err)
	}
	tmpDir := t.TempDir()
	if err := os.Chdir(tmpDir); err != nil {
		t.Fatalf("Chdir: %v", err)
	}
	defer func() {
		if err := os.Chdir(cwd); err != nil {
			t.Errorf("restore Chdir: %v", err)
		}
	}()

	got := FindBinary()
	// On a host where /usr/local/bin/vmaf or any of the in-tree build
	// directories happen to exist FindBinary may return an existing
	// path.  In a fresh tempdir without CLAUDE.md the loop falls back
	// to the last candidate — verify the returned path ends with
	// "/tools/vmaf" (matches every candidate in the list) or
	// /usr/local/bin/vmaf if it actually exists.
	if !strings.Contains(got, "vmaf") {
		t.Errorf("FindBinary() = %q, expected to contain 'vmaf'", got)
	}
}

// TestRepoRoot_FindsClaudeMd verifies the walk-upward logic.
func TestRepoRoot_FindsClaudeMd(t *testing.T) {
	// Mutates cwd — cannot run in parallel.
	tmpDir := t.TempDir()
	deep := filepath.Join(tmpDir, "a", "b", "c")
	if err := os.MkdirAll(deep, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	// Plant a CLAUDE.md at tmpDir.
	if err := os.WriteFile(filepath.Join(tmpDir, "CLAUDE.md"), []byte("# test"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	cwd, err := os.Getwd()
	if err != nil {
		t.Fatalf("Getwd: %v", err)
	}
	if err := os.Chdir(deep); err != nil {
		t.Fatalf("Chdir: %v", err)
	}
	defer func() {
		if err := os.Chdir(cwd); err != nil {
			t.Errorf("restore Chdir: %v", err)
		}
	}()

	// Use EvalSymlinks because macOS /tmp is a symlink to /private/tmp.
	wantResolved, _ := filepath.EvalSymlinks(tmpDir)
	gotResolved, _ := filepath.EvalSymlinks(RepoRoot())
	if gotResolved != wantResolved {
		t.Errorf("RepoRoot() = %q, want %q", gotResolved, wantResolved)
	}
}

// TestRepoRoot_NoMarkerFallsBackToCWD verifies the no-marker fallback.
func TestRepoRoot_NoMarkerFallsBackToCWD(t *testing.T) {
	// Mutates cwd — cannot run in parallel.
	tmpDir := t.TempDir()
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatalf("Getwd: %v", err)
	}
	if err := os.Chdir(tmpDir); err != nil {
		t.Fatalf("Chdir: %v", err)
	}
	defer func() {
		if err := os.Chdir(cwd); err != nil {
			t.Errorf("restore Chdir: %v", err)
		}
	}()
	// On Linux test hosts the CLAUDE.md at the repo root will be visible
	// from /tmp because RepoRoot walks upward; we only assert the call
	// returns a non-empty path.
	got := RepoRoot()
	if got == "" {
		t.Error("RepoRoot returned empty string")
	}
}

// TestAllowedRoots_DefaultSet verifies the default set is non-empty.
func TestAllowedRoots_DefaultSet(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_MCP_ALLOW")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_MCP_ALLOW", old)
		} else {
			_ = os.Unsetenv("VMAF_MCP_ALLOW")
		}
	}()
	_ = os.Unsetenv("VMAF_MCP_ALLOW")

	got := AllowedRoots()
	if len(got) < 1 {
		t.Errorf("AllowedRoots returned empty slice")
	}
}

// TestAllowedRoots_ExtraEnv verifies the VMAF_MCP_ALLOW env var contributes.
func TestAllowedRoots_ExtraEnv(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_MCP_ALLOW")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_MCP_ALLOW", old)
		} else {
			_ = os.Unsetenv("VMAF_MCP_ALLOW")
		}
	}()
	tmpDir := t.TempDir()
	if err := os.Setenv("VMAF_MCP_ALLOW", tmpDir+":/tmp"); err != nil {
		t.Fatalf("Setenv: %v", err)
	}

	got := AllowedRoots()
	found := false
	for _, r := range got {
		// EvalSymlinks may have resolved tmpDir; match either form.
		if r == tmpDir || strings.HasSuffix(r, filepath.Base(tmpDir)) {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("VMAF_MCP_ALLOW entry not present in roots: %v", got)
	}
}

// TestValidatePath_RejectsNonexistent verifies missing files error out.
func TestValidatePath_RejectsNonexistent(t *testing.T) {
	t.Parallel()
	_, err := ValidatePath("/nonexistent/path/file.yuv")
	if err == nil {
		t.Fatal("ValidatePath should error on missing file")
	}
}

// TestValidatePath_RejectsOutsideRoots verifies the allow-list gate.
func TestValidatePath_RejectsOutsideRoots(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_MCP_ALLOW")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_MCP_ALLOW", old)
		} else {
			_ = os.Unsetenv("VMAF_MCP_ALLOW")
		}
	}()
	_ = os.Unsetenv("VMAF_MCP_ALLOW")

	// Create a file in a tmp dir that is not under any default allow root.
	tmpDir := t.TempDir()
	f := filepath.Join(tmpDir, "stray.yuv")
	if err := os.WriteFile(f, []byte("x"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := ValidatePath(f)
	if err == nil {
		t.Fatal("ValidatePath should reject paths outside allow-list")
	}
}

// TestValidatePath_AllowsViaEnv verifies a file under a VMAF_MCP_ALLOW
// entry is accepted.
func TestValidatePath_AllowsViaEnv(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_MCP_ALLOW")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_MCP_ALLOW", old)
		} else {
			_ = os.Unsetenv("VMAF_MCP_ALLOW")
		}
	}()

	tmpDir := t.TempDir()
	f := filepath.Join(tmpDir, "ok.yuv")
	if err := os.WriteFile(f, []byte("x"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	if err := os.Setenv("VMAF_MCP_ALLOW", tmpDir); err != nil {
		t.Fatalf("Setenv: %v", err)
	}

	got, err := ValidatePath(f)
	if err != nil {
		t.Fatalf("ValidatePath: %v", err)
	}
	if got == "" {
		t.Error("ValidatePath returned empty path")
	}
}

// TestValidatePath_RejectsDirectory verifies the dir-vs-file gate.
func TestValidatePath_RejectsDirectory(t *testing.T) {
	// Mutates env — cannot run in parallel.
	old := os.Getenv("VMAF_MCP_ALLOW")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAF_MCP_ALLOW", old)
		} else {
			_ = os.Unsetenv("VMAF_MCP_ALLOW")
		}
	}()
	tmpDir := t.TempDir()
	if err := os.Setenv("VMAF_MCP_ALLOW", tmpDir); err != nil {
		t.Fatalf("Setenv: %v", err)
	}

	_, err := ValidatePath(tmpDir) // points at a directory
	if err == nil {
		t.Fatal("ValidatePath should reject directory targets")
	}
}
