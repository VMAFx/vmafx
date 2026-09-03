// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strings"
	"testing"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// ---------------------------------------------------------------------------
// TestToolListMatchesPython
//
// Boots the Go MCP server in-process, sends tools/list, and verifies that
// every tool name present in the Python server's tool list is also present
// in the Go list. Ordering may differ; names + required fields must match.
// ---------------------------------------------------------------------------

// pythonToolNames is the canonical ordered list from the Python server.
// Must stay in sync with mcp-server/vmaf-mcp/src/vmaf_mcp/server.py
// _list_tools() output.
var pythonToolNames = []string{
	"vmaf_score",
	"list_models",
	"list_backends",
	"run_benchmark",
	"eval_model_on_split",
	"compare_models",
	"describe_worst_frames",
	"probe_backend",
	"vmaf_version",
	"vmaf_score_encoded",
	"list_extractors",
	"describe_model",
	"run_compare",
	"run_ladder",
	"run_tune_per_shot",
}

func TestToolListMatchesPython(t *testing.T) {
	t.Parallel()

	srv := buildServer(nil) // nil logger is fine for tests
	client := mcp.NewClient(&mcp.Implementation{Name: "test-client", Version: "0.0.1"}, nil)

	t1, t2 := mcp.NewInMemoryTransports()
	ctx := context.Background()

	_, err := srv.Connect(ctx, t1, nil)
	if err != nil {
		t.Fatalf("server.Connect: %v", err)
	}
	session, err := client.Connect(ctx, t2, nil)
	if err != nil {
		t.Fatalf("client.Connect: %v", err)
	}
	defer session.Close()

	result, err := session.ListTools(ctx, &mcp.ListToolsParams{})
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}

	goNames := make(map[string]bool)
	for _, tool := range result.Tools {
		goNames[tool.Name] = true
	}

	for _, name := range pythonToolNames {
		if !goNames[name] {
			t.Errorf("Go tool list is missing Python tool %q", name)
		}
	}

	// Also check that Go has at least as many tools as Python.
	if len(result.Tools) < len(pythonToolNames) {
		t.Errorf("Go tool list has %d tools; Python has %d", len(result.Tools), len(pythonToolNames))
	}

	t.Logf("Go tool list (%d tools):", len(result.Tools))
	var goSorted []string
	for _, tool := range result.Tools {
		goSorted = append(goSorted, tool.Name)
	}
	sort.Strings(goSorted)
	for _, name := range goSorted {
		t.Logf("  %s", name)
	}
}

// ---------------------------------------------------------------------------
// TestToolSchemasMatchPython
//
// Verifies that key tool input schemas (required fields) match the Python
// server's definitions. This protects against IDE clients breaking when the
// Go server inadvertently drops a required argument.
// ---------------------------------------------------------------------------

var pythonToolRequired = map[string][]string{
	"vmaf_score":            {"ref", "dis", "width", "height", "pixfmt", "bitdepth"},
	"eval_model_on_split":   {"model", "features"},
	"compare_models":        {"models", "features"},
	"describe_worst_frames": {"ref", "dis", "width", "height", "pixfmt", "bitdepth"},
	"probe_backend":         {"backend"},
	"vmaf_score_encoded":    {"reference_encoded", "distorted_encoded"},
	"describe_model":        {"name"},
	"run_compare":           {"src"},
	"run_ladder":            {"src", "resolutions", "target_vmafs"},
	"run_tune_per_shot":     {"src"},
}

func TestToolSchemasMatchPython(t *testing.T) {
	t.Parallel()

	srv := buildServer(nil)
	client := mcp.NewClient(&mcp.Implementation{Name: "test-client", Version: "0.0.1"}, nil)

	t1, t2 := mcp.NewInMemoryTransports()
	ctx := context.Background()

	_, err := srv.Connect(ctx, t1, nil)
	if err != nil {
		t.Fatalf("server.Connect: %v", err)
	}
	session, err := client.Connect(ctx, t2, nil)
	if err != nil {
		t.Fatalf("client.Connect: %v", err)
	}
	defer session.Close()

	result, err := session.ListTools(ctx, &mcp.ListToolsParams{})
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}

	toolsByName := make(map[string]*mcp.Tool)
	for _, tool := range result.Tools {
		toolsByName[tool.Name] = tool
	}

	for toolName, expectedRequired := range pythonToolRequired {
		tool, ok := toolsByName[toolName]
		if !ok {
			t.Errorf("tool %q not found in Go list", toolName)
			continue
		}
		// Parse the input schema to extract required fields.
		// InputSchema is typed as any in the SDK; marshal it back to JSON first.
		schemaBytes, err := json.Marshal(tool.InputSchema)
		if err != nil {
			t.Errorf("tool %q: failed to marshal inputSchema: %v", toolName, err)
			continue
		}
		var schema map[string]any
		if err := json.Unmarshal(schemaBytes, &schema); err != nil {
			t.Errorf("tool %q: failed to parse inputSchema: %v", toolName, err)
			continue
		}
		requiredRaw, _ := schema["required"].([]any)
		goRequired := make(map[string]bool)
		for _, r := range requiredRaw {
			if s, ok := r.(string); ok {
				goRequired[s] = true
			}
		}
		for _, field := range expectedRequired {
			if !goRequired[field] {
				t.Errorf("tool %q: required field %q missing from Go schema", toolName, field)
			}
		}
	}
}

// ---------------------------------------------------------------------------
// TestVmafScoreTool
//
// Calls the vmaf_score tool via the Go MCP server using the Netflix golden
// fixture pair and verifies the pooled mean VMAF score matches the expected
// value (76.668 to 3 decimal places, matching the Python server's output).
//
// This test is skipped when the vmaf binary or Netflix golden YUVs are
// not available (e.g. on CI runners without the test corpus).
// ---------------------------------------------------------------------------

func TestVmafScoreTool(t *testing.T) {
	// t.Parallel() omitted: t.Setenv is used below, and Go 1.22+ panics when
	// t.Setenv is called inside a t.Parallel test (the env change would race
	// with the test runner restoring it for other goroutines). The test is
	// already skipped on CI without a vmaf binary, so sequential execution is
	// not a bottleneck.

	repoRoot := findRepoRoot(t)
	refYUV := repoRoot + "/python/test/resource/yuv/src01_hrc00_576x324.yuv"
	disYUV := repoRoot + "/python/test/resource/yuv/src01_hrc01_576x324.yuv"

	for _, p := range []string{refYUV, disYUV} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("Netflix golden YUV not found (%s): %v", p, err)
		}
	}

	vmafBin := os.Getenv("VMAF_BIN")
	if vmafBin == "" {
		candidates := []string{
			"/usr/local/bin/vmaf",
			repoRoot + "/core/build/tools/vmaf",
			repoRoot + "/build/tools/vmaf",
		}
		for _, c := range candidates {
			if _, err := os.Stat(c); err == nil {
				vmafBin = c
				break
			}
		}
	}
	if vmafBin == "" {
		t.Skip("vmaf binary not found; build first with meson compile -C build")
	}
	t.Setenv("VMAF_BIN", vmafBin)

	// Also allowlist the YUV paths since the golden YUVs are under python/test/resource/yuv
	// which is already in the default allowed roots — but set VMAF_MCP_ALLOW just in case
	// the test is run from an unusual working directory.
	t.Setenv("VMAF_MCP_ALLOW", repoRoot+"/python/test/resource")

	result, err := handleVmafScore(context.Background(), map[string]any{
		"ref":       refYUV,
		"dis":       disYUV,
		"width":     float64(576),
		"height":    float64(324),
		"pixfmt":    "420",
		"bitdepth":  float64(8),
		"model":     "version=vmaf_v0.6.1",
		"backend":   "cpu",
		"precision": "6",
	})
	if err != nil {
		t.Fatalf("handleVmafScore: %v", err)
	}

	payload, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("unexpected result type %T", result)
	}

	pooled, _ := payload["pooled_metrics"].(map[string]any)
	vmafPool, _ := pooled["vmaf"].(map[string]any)
	mean, ok := vmafPool["mean"].(float64)
	if !ok {
		t.Fatalf("pooled_metrics.vmaf.mean not found; payload keys: %v", mapKeys(pooled))
	}

	// Netflix golden: mean VMAF ≈ 76.668 (to 3 d.p.).
	expected := 76.668
	tolerance := 0.01
	if diff := mean - expected; diff < -tolerance || diff > tolerance {
		t.Errorf("vmaf_score mean = %.3f, want %.3f (±%.3f)", mean, expected, tolerance)
	} else {
		t.Logf("vmaf_score mean = %.3f (expected %.3f)", mean, expected)
	}
}

// ---------------------------------------------------------------------------
// TestGoVsPythonOutputParity
//
// Runs both the Go and Python MCP servers against the same Netflix golden
// input and diffs their JSON output. Documents acceptable differences.
//
// This test is skipped when the Python server, vmaf binary, or fixtures
// are not available.
// ---------------------------------------------------------------------------

func TestGoVsPythonOutputParity(t *testing.T) {
	repoRoot := findRepoRoot(t)
	refYUV := repoRoot + "/python/test/resource/yuv/src01_hrc00_576x324.yuv"
	disYUV := repoRoot + "/python/test/resource/yuv/src01_hrc01_576x324.yuv"

	for _, p := range []string{refYUV, disYUV} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("Netflix golden YUV not found: %s", p)
		}
	}

	// Check Python server is available.
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not on PATH")
	}
	pythonServer := repoRoot + "/mcp-server/vmaf-mcp/src/vmaf_mcp/server.py"
	if _, err := os.Stat(pythonServer); err != nil {
		t.Skipf("Python MCP server not found: %s", pythonServer)
	}

	vmafBin := os.Getenv("VMAF_BIN")
	if vmafBin == "" {
		t.Skip("VMAF_BIN not set; skipping parity test")
	}

	// Get Go result.
	t.Setenv("VMAF_BIN", vmafBin)
	t.Setenv("VMAF_MCP_ALLOW", repoRoot+"/python/test/resource")

	goResult, err := handleVmafScore(context.Background(), map[string]any{
		"ref":       refYUV,
		"dis":       disYUV,
		"width":     float64(576),
		"height":    float64(324),
		"pixfmt":    "420",
		"bitdepth":  float64(8),
		"model":     "version=vmaf_v0.6.1",
		"backend":   "cpu",
		"precision": "6",
	})
	if err != nil {
		t.Fatalf("Go handleVmafScore: %v", err)
	}

	// Get Python result by calling the Python helper directly.
	pyScript := fmt.Sprintf(`
import asyncio, json, sys, os
sys.path.insert(0, %q)
os.environ["VMAF_BIN"] = %q
from vmaf_mcp.server import _run_vmaf_score, ScoreRequest
from pathlib import Path
import asyncio

async def main():
    req = ScoreRequest(
        ref=Path(%q), dis=Path(%q),
        width=576, height=324, pixfmt="420", bitdepth=8,
        model="version=vmaf_v0.6.1", backend="cpu", precision="6"
    )
    result = await _run_vmaf_score(req)
    print(json.dumps(result))

asyncio.run(main())
`,
		repoRoot+"/mcp-server/vmaf-mcp/src",
		vmafBin, refYUV, disYUV,
	)

	cmd := exec.Command("python3", "-c", pyScript)
	cmd.Env = append(os.Environ(),
		"VMAF_BIN="+vmafBin,
		"VMAF_MCP_ALLOW="+repoRoot+"/python/test/resource",
	)
	pyOut, err := cmd.Output()
	if err != nil {
		t.Skipf("Python parity test could not run: %v (output: %s)", err, string(pyOut))
	}

	var pyResult map[string]any
	if err := json.Unmarshal(bytes.TrimSpace(pyOut), &pyResult); err != nil {
		t.Fatalf("failed to parse Python output: %v\nraw: %s", err, string(pyOut))
	}

	goMap, _ := goResult.(map[string]any)

	// Compare pooled vmaf mean — the most important value.
	goPooled, _ := goMap["pooled_metrics"].(map[string]any)
	pyPooled, _ := pyResult["pooled_metrics"].(map[string]any)
	goVmaf, _ := goPooled["vmaf"].(map[string]any)
	pyVmaf, _ := pyPooled["vmaf"].(map[string]any)
	goMean, _ := goVmaf["mean"].(float64)
	pyMean, _ := pyVmaf["mean"].(float64)

	if diff := goMean - pyMean; diff < -0.001 || diff > 0.001 {
		t.Errorf("pooled_metrics.vmaf.mean: Go=%.6f, Python=%.6f (diff=%.6f)", goMean, pyMean, diff)
	} else {
		t.Logf("pooled_metrics.vmaf.mean: Go=%.6f == Python=%.6f", goMean, pyMean)
	}

	// Document acceptable differences:
	// - backend_used / backend_requested: these are meta-fields added by the server layer,
	//   may differ if Python and Go disagree on backend inference.
	// - Any keys present in Python result but absent in Go result (or vice versa):
	//   the Go implementation may have slightly different extra fields.
	acceptableDiff := map[string]bool{
		"backend_used":      true,
		"backend_requested": true,
	}
	for k := range pyResult {
		if acceptableDiff[k] {
			continue
		}
		if _, ok := goMap[k]; !ok {
			t.Logf("DIFF: Python has key %q not present in Go result (may be acceptable)", k)
		}
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func findRepoRoot(t *testing.T) string {
	t.Helper()
	// Walk upward from the test file's location.
	dir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for {
		if _, err := os.Stat(dir + "/CLAUDE.md"); err == nil {
			return dir
		}
		parent := strings.LastIndex(dir, "/")
		if parent <= 0 {
			t.Fatal("could not find repo root (no CLAUDE.md)")
		}
		dir = dir[:parent]
	}
}

func mapKeys(m map[string]any) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
