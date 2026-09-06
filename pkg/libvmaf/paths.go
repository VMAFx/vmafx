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
// discoverRepoRoot walks upward from the current working directory looking for
// the CLAUDE.md marker. It returns (dir, true) when the marker is found, or
// ("", false) when the walk reaches the filesystem root without finding it (or
// the cwd cannot be determined). Callers that must fail closed — e.g.
// AllowedRoots — use this directly instead of RepoRoot's best-effort fallback.
func discoverRepoRoot() (string, bool) {
	cwd, err := os.Getwd()
	if err != nil {
		return "", false
	}
	dir := cwd
	for {
		if _, err := os.Stat(filepath.Join(dir, "CLAUDE.md")); err == nil {
			return dir, true
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", false
		}
		dir = parent
	}
}

func RepoRoot() string {
	if root, ok := discoverRepoRoot(); ok {
		return root
	}
	// Best-effort fallback for path-joining callers when the marker is absent.
	// NOTE: this fallback is deliberately NOT used by AllowedRoots (which must
	// fail closed) — see discoverRepoRoot.
	if cwd, err := os.Getwd(); err == nil {
		return cwd
	}
	return "."
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
	roots := []string{}
	// Add the repo-relative roots ONLY when an actual repo root (CLAUDE.md
	// marker) was found. RepoRoot's cwd fallback must not be used here: if the
	// server runs outside the repo it would allowlist arbitrary cwd-relative
	// trees (<cwd>/testdata, ...). Fail closed instead, mirroring the C
	// discover_repo_root() guard in core/src/mcp/compute_vmaf.c.
	if root, ok := discoverRepoRoot(); ok {
		roots = append(roots,
			filepath.Join(root, "testdata"),
			filepath.Join(root, "python", "test", "resource"),
			filepath.Join(root, "python", "test", "resource", "yuv"),
			filepath.Join(root, "model"),
		)
	}
	// The container mount is always allowed regardless of repo discovery.
	roots = append(roots, "/workspace/python/test/resource")
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

// ---------------------------------------------------------------------------
// Sidecar binaries (vmaf-perShot / vmaf_roi / vmaf_bench / vmaf_vpl)
// ---------------------------------------------------------------------------

// SidecarBinaryEnv maps each sidecar tool's on-disk binary name to the
// environment variable that overrides its location.
//
// The names are the meson target names in core/tools/meson.build — note the
// deliberate camelCase of "vmaf-perShot", which is the installed binary name.
var SidecarBinaryEnv = map[string]string{
	"vmaf-perShot": "VMAF_PER_SHOT_BIN",
	"vmaf_roi":     "VMAF_ROI_BIN",
	"vmaf_bench":   "VMAF_BENCH_BIN",
	"vmaf_vpl":     "VMAF_VPL_BIN",
}

// FindSidecarBinary returns the path to one of the sidecar CLI binaries built
// alongside the main `vmaf` tool.
//
// Resolution order:
//  1. The tool's own environment override (see SidecarBinaryEnv).
//  2. A sibling of the resolved `vmaf` binary (FindBinary). This is what makes
//     VMAF_BIN=<somewhere>/tools/vmaf resolve the whole family, and it is the
//     case in the vmaf-dev-mcp container after `make install`.
//  3. /usr/local/bin/<name>.
//  4. <repoRoot>/core/build/tools/<name> (in-tree build after ADR-0700).
//  5. <repoRoot>/build/tools/<name> (legacy build-dir name).
//
// Returns the first path that exists on disk; when none exist the last
// candidate is returned so the caller can emit a clear "build first" error.
// An unknown name returns "" — callers must treat that as a programming error.
func FindSidecarBinary(name string) string {
	env, known := SidecarBinaryEnv[name]
	if !known {
		return ""
	}
	if v := os.Getenv(env); v != "" {
		return v
	}
	root := RepoRoot()
	candidates := []string{
		filepath.Join(filepath.Dir(FindBinary()), name),
		filepath.Join("/usr/local/bin", name),
		filepath.Join(root, "core", "build", "tools", name),
		filepath.Join(root, "build", "tools", name),
	}
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	return candidates[len(candidates)-1]
}

// ValidateDir is the directory-shaped counterpart of ValidatePath: it resolves
// p, confirms it is under one of the AllowedRoots, and confirms it is a
// directory. Used by tool handlers that take a data-directory argument
// (vmaf_bench --data-dir) where ValidatePath's "must be a regular file" check
// would reject every legitimate value.
func ValidateDir(p string) (string, error) {
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", fmt.Errorf("cannot resolve path %q: %w", p, err)
	}
	resolved, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return "", fmt.Errorf("directory not found: %s", abs)
	}
	for _, root := range AllowedRoots() {
		rootClean := filepath.Clean(root) + string(filepath.Separator)
		if strings.HasPrefix(filepath.Clean(resolved)+string(filepath.Separator), rootClean) {
			fi, err := os.Stat(resolved)
			if err != nil {
				return "", fmt.Errorf("directory not found: %s", resolved)
			}
			if !fi.IsDir() {
				return "", fmt.Errorf("path is a file, not a directory: %s", resolved)
			}
			return resolved, nil
		}
	}
	return "", fmt.Errorf("path %s is not under an allowlisted root; set VMAF_MCP_ALLOW to extend", resolved)
}
