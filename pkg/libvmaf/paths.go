// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package libvmaf

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// FindBinary returns the path to the vmaf CLI binary.
//
// Resolution order:
//  1. VMAF_BIN environment variable (explicit override).
//  2. /usr/local/bin/vmaf (installed by make install or the container).
//  3. <repoRoot>/core/build/tools/vmaf (in-tree fork build after ADR-0700).
//  4. <repoRoot>/build/tools/vmaf (legacy build-dir name).
//
// Returns the first path that exists on disk. If none exist the
// last candidate path is returned so the caller can emit a clear error.
func FindBinary() string {
	if env := os.Getenv("VMAF_BIN"); env != "" {
		return env
	}
	root := RepoRoot()
	candidates := []string{
		"/usr/local/bin/vmaf",
		filepath.Join(root, "core", "build", "tools", "vmaf"),
		filepath.Join(root, "build", "tools", "vmaf"),
	}
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	return candidates[len(candidates)-1]
}

// RepoRoot returns the repository root directory by walking up from this
// source file at compile time (via //go:generate) or from the process
// working directory at runtime.
//
// The function looks for the presence of a CLAUDE.md file (which exists at
// the repository root) to confirm the root rather than relying on a fixed
// relative-path assumption.
func RepoRoot() string {
	// Walk from the current working directory upward.
	cwd, err := os.Getwd()
	if err != nil {
		return "."
	}
	dir := cwd
	for {
		if _, err := os.Stat(filepath.Join(dir, "CLAUDE.md")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			// Reached filesystem root without finding the marker; fall back
			// to the working directory.
			return cwd
		}
		dir = parent
	}
}

// AllowedRoots returns the set of filesystem trees under which MCP tool paths
// are allowed.
//
// Default roots mirror the Python server:
//   - <repo>/testdata
//   - <repo>/python/test/resource
//   - <repo>/model
//   - /workspace/python/test/resource (vmaf-dev-mcp container mount)
//
// Additional roots may be added at runtime via the VMAF_MCP_ALLOW environment
// variable (colon-separated list of absolute paths).
func AllowedRoots() []string {
	root := RepoRoot()
	roots := []string{
		filepath.Join(root, "testdata"),
		filepath.Join(root, "python", "test", "resource"),
		filepath.Join(root, "model"),
		"/workspace/python/test/resource",
	}
	if extra := os.Getenv("VMAF_MCP_ALLOW"); extra != "" {
		// filepath.SplitList uses the OS path-list separator (':' on Unix,
		// ';' on Windows), which correctly handles Windows drive letters
		// such as C:\foo that would be mis-split by a hardcoded ':'.
		for _, p := range filepath.SplitList(extra) {
			if p != "" {
				abs, err := filepath.Abs(p)
				if err == nil {
					roots = append(roots, abs)
				}
			}
		}
	}
	resolved := make([]string, 0, len(roots))
	for _, r := range roots {
		abs, err := filepath.EvalSymlinks(r)
		if err != nil {
			// If the path does not exist yet, keep the unresolved form.
			abs = r
		}
		resolved = append(resolved, abs)
	}
	return resolved
}

// ValidatePath resolves p and confirms it is under one of the allowed roots
// and exists as a file on disk.
//
// Returns the resolved absolute path on success. Returns an error when p is
// outside every allowed root or when the file does not exist.
func ValidatePath(p string) (string, error) {
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", fmt.Errorf("cannot resolve path %q: %w", p, err)
	}
	resolved, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return "", fmt.Errorf("file not found: %s", abs)
	}
	for _, root := range AllowedRoots() {
		rootClean := filepath.Clean(root) + string(filepath.Separator)
		if strings.HasPrefix(filepath.Clean(resolved)+string(filepath.Separator), rootClean) {
			// Confirm it is actually a file.
			fi, err := os.Stat(resolved)
			if err != nil {
				return "", fmt.Errorf("file not found: %s", resolved)
			}
			if fi.IsDir() {
				return "", fmt.Errorf("path is a directory, not a file: %s", resolved)
			}
			return resolved, nil
		}
	}
	return "", fmt.Errorf("path %s is not under an allowlisted root; set VMAF_MCP_ALLOW to extend", resolved)
}
