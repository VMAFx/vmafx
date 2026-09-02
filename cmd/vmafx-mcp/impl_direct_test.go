// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// impl_direct_test.go — unit tests for the MCP-side dispatch wiring that
// chooses between the subprocess path (default) and the direct-cgo path
// (VMAFX_MCP_DIRECT=1).  ADR-0931 Phase 1.

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDirectPathEnabled(t *testing.T) {
	cases := []struct {
		val  string
		want bool
	}{
		{"", false},
		{"0", false},
		{"false", false},
		{"true", false}, // strict: only "1" enables, mirrors Phase 1 contract
		{"1", true},
	}
	for _, tc := range cases {
		t.Run("env="+tc.val, func(t *testing.T) {
			t.Setenv("VMAFX_MCP_DIRECT", tc.val)
			if got := directPathEnabled(); got != tc.want {
				t.Errorf("directPathEnabled() = %v, want %v (env=%q)", got, tc.want, tc.val)
			}
		})
	}
}

func TestResolveModelArgToPath(t *testing.T) {
	root := findRepoRoot(t)
	modelsDir := filepath.Join(root, "model")
	cases := []struct {
		name    string
		in      string
		wantSub string // substring the resolved path must contain
		wantErr bool
	}{
		{"version_prefix", "version=vmaf_v0.6.1", "vmaf_v0.6.1.json", false},
		{"bare_stem", "vmaf_v0.6.1", "vmaf_v0.6.1.json", false},
		{"full_filename", "vmaf_v0.6.1.json", "vmaf_v0.6.1.json", false},
		{"abs_path", filepath.Join(modelsDir, "vmaf_v0.6.1.json"), "vmaf_v0.6.1.json", false},
		{"path_prefix", "path=" + filepath.Join(modelsDir, "vmaf_v0.6.1.json"), "vmaf_v0.6.1.json", false},
		{"missing", "nonexistent_model_xyz", "", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			// Skip path-existence-dependent cases when the model file is
			// absent (e.g. CI runner without the model JSONs vendored).
			modelExists := false
			if _, err := os.Stat(filepath.Join(modelsDir, "vmaf_v0.6.1.json")); err == nil {
				modelExists = true
			}
			if !modelExists && !tc.wantErr {
				t.Skip("vmaf_v0.6.1.json not present in model/; skipping path-existence test")
			}
			got, err := resolveModelArgToPath(tc.in)
			if tc.wantErr {
				if err == nil {
					t.Errorf("expected error for %q, got nil (path=%q)", tc.in, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if !strings.Contains(got, tc.wantSub) {
				t.Errorf("resolved path %q does not contain %q", got, tc.wantSub)
			}
		})
	}
}

// TestResolveModelArgToPath_AllowlistEnforced verifies that resolveModelArgToPath
// rejects paths outside the allowlisted roots, even when path= or an absolute path
// is supplied directly.  This is the regression test for the bypass where an absolute
// path was returned without ValidatePath validation.
func TestResolveModelArgToPath_AllowlistEnforced(t *testing.T) {
	// Create a file outside any allowed root (in a temp dir).
	tmpDir := t.TempDir()
	outsideFile := filepath.Join(tmpDir, "evil_model.json")
	if err := os.WriteFile(outsideFile, []byte(`{"model_dict":{}}`), 0o600); err != nil {
		t.Fatalf("setup: %v", err)
	}

	// Ensure the temp dir is NOT in VMAF_MCP_ALLOW.
	t.Setenv("VMAF_MCP_ALLOW", "")

	// Absolute path form — must be rejected.
	if got, err := resolveModelArgToPath(outsideFile); err == nil {
		t.Errorf("resolveModelArgToPath(%q) succeeded with path=%q; expected allowlist rejection", outsideFile, got)
	}

	// path= prefix form — must also be rejected.
	if got, err := resolveModelArgToPath("path=" + outsideFile); err == nil {
		t.Errorf("resolveModelArgToPath(%q) succeeded with path=%q; expected allowlist rejection", "path="+outsideFile, got)
	}
}

// TestHandleVmafScore_RoutesToDirect verifies that flipping VMAFX_MCP_DIRECT
// changes the code path taken — the subprocess path fails fast when the
// vmaf binary is absent (no VMAF_BIN, no /usr/local/bin/vmaf), while the
// direct path returns the libvmaf-native error.  We do not assert on the
// score itself here (the end-to-end check lives in TestVmafScoreTool).
func TestHandleVmafScore_RoutesToDirect(t *testing.T) {
	root := findRepoRoot(t)
	ref := filepath.Join(root, "testdata", "ref_576x324_48f.yuv")
	dis := filepath.Join(root, "testdata", "dis_576x324_48f.yuv")
	model := filepath.Join(root, "model", "vmaf_v0.6.1.json")
	for _, p := range []string{ref, dis, model} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s); skipping", p)
		}
	}
	t.Setenv("VMAFX_MCP_DIRECT", "1")
	t.Setenv("VMAF_MCP_ALLOW", filepath.Join(root, "testdata"))

	res, err := handleVmafScore(context.Background(), map[string]any{
		"ref":      ref,
		"dis":      dis,
		"width":    float64(576),
		"height":   float64(324),
		"pixfmt":   "420",
		"bitdepth": float64(8),
		"model":    "version=vmaf_v0.6.1",
		"backend":  "cpu",
	})
	if err != nil {
		t.Fatalf("handleVmafScore (direct): %v", err)
	}
	payload, ok := res.(map[string]any)
	if !ok {
		t.Fatalf("unexpected payload type %T", res)
	}
	used, _ := payload["backend_used"].(string)
	if !strings.Contains(used, "direct cgo") {
		t.Errorf("backend_used = %q, want substring 'direct cgo'", used)
	}
	fc, _ := payload["frame_count"].(int)
	if fc != 48 {
		t.Errorf("frame_count = %d, want 48", fc)
	}
}

// TestHandleDescribeModel_DirectAugments verifies that the direct describe
// path adds the libvmaf_validated marker when VMAFX_MCP_DIRECT=1 and the
// model loads through libvmaf.  Skipped when the model file is absent.
func TestHandleDescribeModel_DirectAugments(t *testing.T) {
	root := findRepoRoot(t)
	model := filepath.Join(root, "model", "vmaf_v0.6.1.json")
	if _, err := os.Stat(model); err != nil {
		t.Skipf("vmaf_v0.6.1.json not present; skipping")
	}
	t.Setenv("VMAFX_MCP_DIRECT", "1")
	res, err := handleDescribeModel(context.Background(), map[string]any{
		"name": "vmaf_v0.6.1",
	})
	if err != nil {
		t.Fatalf("handleDescribeModel (direct): %v", err)
	}
	payload, ok := res.(map[string]any)
	if !ok {
		t.Fatalf("unexpected payload type %T", res)
	}
	if v, ok := payload["libvmaf_validated"].(bool); !ok || !v {
		t.Errorf("libvmaf_validated = %v (want true)", payload["libvmaf_validated"])
	}
}

// TestHandleDescribeModel_SubprocessUnaugmented verifies that the default
// path (subprocess) does NOT touch libvmaf and therefore does not emit the
// libvmaf_validated marker.
func TestHandleDescribeModel_SubprocessUnaugmented(t *testing.T) {
	root := findRepoRoot(t)
	model := filepath.Join(root, "model", "vmaf_v0.6.1.json")
	if _, err := os.Stat(model); err != nil {
		t.Skipf("vmaf_v0.6.1.json not present; skipping")
	}
	t.Setenv("VMAFX_MCP_DIRECT", "")
	res, err := handleDescribeModel(context.Background(), map[string]any{
		"name": "vmaf_v0.6.1",
	})
	if err != nil {
		t.Fatalf("handleDescribeModel (subprocess): %v", err)
	}
	payload, ok := res.(map[string]any)
	if !ok {
		t.Fatalf("unexpected payload type %T", res)
	}
	if _, ok := payload["libvmaf_validated"]; ok {
		t.Errorf("subprocess path unexpectedly emitted libvmaf_validated marker")
	}
}
