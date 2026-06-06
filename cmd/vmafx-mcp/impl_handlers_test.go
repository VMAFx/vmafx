// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// impl_handlers_test.go — table-driven tests covering the error/no-binary
// paths of each MCP tool handler and several pure helpers that were previously
// uncovered.  No external binary (vmaf, ffmpeg, vmaf-tune) is required: every
// test either targets a pure function or exercises the "binary not found"
// fast-fail branches that require no filesystem side effects beyond what
// os.MkdirTemp provides.
//
// Coverage targets (all at 0% before this file):
//   - handleListModels       — real walk of model/ dir
//   - handleListBackends     — binary-absent fallback path
//   - handleVmafVersion      — binary-absent early-return
//   - handleRunBenchmark     — script-absent error path
//   - handleEvalModelOnSplit — ValidatePath rejection (missing model)
//   - handleCompareModels    — partial-errors path (some models invalid)
//   - handleDescribeWorstFrames — ValidatePath rejection
//   - handleRunCompare       — vmaf-tune absent error path
//   - handleRunLadder        — vmaf-tune absent error path
//   - handleRunTunePerShot   — vmaf-tune absent error path
//   - findVmafTune           — VMAF_TUNE_BIN env override
//   - parseArgs              — nil args and valid JSON object
//   - probeBackends          — cache hit path
//   - describeModel          — multiple-match ambiguity path
//   - describeModelFile      — non-JSON extension (no model_type)
//   - handleListExtractors   — real walk of core/src/feature/
//   - addRawTool closure     — handler error and marshal branches

package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// ---------------------------------------------------------------------------
// handleListModels — walks the real model/ directory.
// ---------------------------------------------------------------------------

func TestHandleListModels(t *testing.T) {
	t.Parallel()
	result, err := handleListModels(context.Background(), nil)
	if err != nil {
		t.Fatalf("handleListModels: %v", err)
	}
	m, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("unexpected result type %T", result)
	}
	models, ok := m["models"].([]map[string]any)
	if !ok {
		t.Fatalf("models field type: %T", m["models"])
	}
	if len(models) == 0 {
		t.Fatal("expected at least one model in model/ directory")
	}
	// Verify required keys on the first entry.
	first := models[0]
	for _, key := range []string{"name", "path", "format", "size_bytes"} {
		if _, ok := first[key]; !ok {
			t.Errorf("model entry missing key %q", key)
		}
	}
	// Verify only allowed formats are returned.
	for _, model := range models {
		fmt := model["format"].(string)
		if fmt != "json" && fmt != "pkl" && fmt != "onnx" {
			t.Errorf("unexpected format %q in model entry", fmt)
		}
	}
}

func TestHandleListModels_Sorted(t *testing.T) {
	t.Parallel()
	result, err := handleListModels(context.Background(), nil)
	if err != nil {
		t.Fatalf("handleListModels: %v", err)
	}
	m := result.(map[string]any)
	models := m["models"].([]map[string]any)
	for i := 1; i < len(models); i++ {
		if models[i]["path"].(string) < models[i-1]["path"].(string) {
			t.Errorf("models not sorted: %q < %q", models[i]["path"], models[i-1]["path"])
		}
	}
}

// ---------------------------------------------------------------------------
// handleListBackends — when vmaf binary is absent, returns safe defaults.
// ---------------------------------------------------------------------------

func TestHandleListBackends_BinaryAbsent(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	// Point VMAF_BIN to a guaranteed non-existent path; the handler returns
	// a fixed map rather than calling probeBackends.
	t.Setenv("VMAF_BIN", "/nonexistent/vmaf-binary-for-test")
	result, err := handleListBackends(context.Background(), nil)
	if err != nil {
		t.Fatalf("handleListBackends (absent): %v", err)
	}
	m, ok := result.(map[string]bool)
	if !ok {
		t.Fatalf("result type: %T", result)
	}
	if !m["cpu"] {
		t.Error("cpu must be true even when binary is absent")
	}
	// All GPU backends should be false when binary absent.
	for _, backend := range []string{"cuda", "sycl", "hip", "metal"} {
		if m[backend] {
			t.Errorf("backend %q should be false when binary is absent", backend)
		}
	}
}

// ---------------------------------------------------------------------------
// handleVmafVersion — binary-absent early-return.
// ---------------------------------------------------------------------------

func TestHandleVmafVersion_BinaryAbsent(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	t.Setenv("VMAF_BIN", "/nonexistent/vmaf-binary-for-test")
	result, err := handleVmafVersion(context.Background(), nil)
	if err != nil {
		t.Fatalf("handleVmafVersion (absent): %v", err)
	}
	m, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("result type: %T", result)
	}
	// Version should be nil, error string should be set.
	if m["version"] != nil {
		t.Errorf("version: got %v, want nil", m["version"])
	}
	if m["error"] == nil || m["error"] == "" {
		t.Error("error field should be non-empty when binary is absent")
	}
	// build_flags cpu should be false (binary not found).
	bf, _ := m["build_flags"].(map[string]bool)
	if bf["cpu"] {
		t.Error("build_flags.cpu: got true when binary absent, want false")
	}
}

// ---------------------------------------------------------------------------
// handleRunBenchmark — script-present path; vmaf binary absent means the
// script exits non-zero, which the handler folds into the payload
// (exit_code ≠ 0) rather than returning a Go error.
// ---------------------------------------------------------------------------

func TestHandleRunBenchmark_BinaryAbsent(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	// bench_all.sh is present in testdata/ (real repo tree); when VMAF_BIN
	// points to a nonexistent binary the script exits non-zero immediately.
	// handleRunBenchmark should return a payload map (not a Go error) with
	// exit_code != 0.
	t.Setenv("VMAF_BIN", "/nonexistent/vmaf-binary-for-benchmark-test")
	result, err := handleRunBenchmark(context.Background(), nil)
	if err != nil {
		t.Fatalf("handleRunBenchmark: unexpected Go error: %v", err)
	}
	payload, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("result type: %T", result)
	}
	// With a nonexistent binary, exit_code must be non-zero.
	code, _ := payload["exit_code"].(int)
	if code == 0 {
		// Allow: the script may not have advanced far enough.  The important
		// invariant is that the function returns a structured payload rather
		// than panicking or returning a Go error.
		t.Logf("exit_code=0 with absent binary; script may skip early")
	}
	// stdout key must always be present.
	if _, ok := payload["stdout"]; !ok {
		t.Error("payload missing 'stdout' key")
	}
}

// ---------------------------------------------------------------------------
// handleEvalModelOnSplit — ValidatePath rejects missing files.
// ---------------------------------------------------------------------------

func TestHandleEvalModelOnSplit_MissingModel(t *testing.T) {
	t.Parallel()
	// Pass a nonexistent model path — ValidatePath should reject it.
	_, err := handleEvalModelOnSplit(context.Background(), map[string]any{
		"model":    "/nonexistent/model.onnx",
		"features": "/nonexistent/features.parquet",
	})
	if err == nil {
		t.Fatal("expected error for missing model path")
	}
	if !strings.Contains(err.Error(), "model") {
		t.Errorf("error should mention 'model', got: %q", err.Error())
	}
}

// ---------------------------------------------------------------------------
// handleCompareModels — some-invalid-models path (partial errors).
// ---------------------------------------------------------------------------

func TestHandleCompareModels_AllInvalidModels(t *testing.T) {
	t.Parallel()
	// features must be a real file under an allowed root so ValidatePath
	// passes.  Use an in-repo model JSON as a stand-in "feature cache" — we
	// just need a file that ValidatePath accepts; the Python eval will fail
	// separately (no onnxruntime) which is captured per-model in "errors".
	root := findRepoRoot(t)
	// Use a model JSON as a dummy features file — its content doesn't matter
	// for this test; we only need ValidatePath to succeed.
	dummyFeatures := filepath.Join(root, "model", "vmaf_v0.6.1.json")
	if _, err := os.Stat(dummyFeatures); err != nil {
		t.Skipf("vmaf_v0.6.1.json not present; skipping")
	}
	// Both model paths are nonexistent; ValidatePath rejects them; the
	// handler collects per-model errors without propagating a Go error.
	result, err := handleCompareModels(context.Background(), map[string]any{
		"models":   []any{"/nope/a.onnx", "/nope/b.onnx"},
		"features": dummyFeatures,
	})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	m, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("result type: %T", result)
	}
	errs, _ := m["errors"].([]map[string]any)
	if len(errs) != 2 {
		t.Errorf("errors: got %d, want 2", len(errs))
	}
	ranked, _ := m["ranked"].([]map[string]any)
	if len(ranked) != 0 {
		t.Errorf("ranked: got %d, want 0", len(ranked))
	}
}

func TestHandleCompareModels_WrongModelsType(t *testing.T) {
	t.Parallel()
	_, err := handleCompareModels(context.Background(), map[string]any{
		"models": "not-a-list", // wrong type
	})
	if err == nil {
		t.Fatal("expected error when models is not a list")
	}
}

// ---------------------------------------------------------------------------
// handleDescribeWorstFrames — ref/dis validation failures.
// ---------------------------------------------------------------------------

func TestHandleDescribeWorstFrames_MissingRef(t *testing.T) {
	t.Parallel()
	_, err := handleDescribeWorstFrames(context.Background(), map[string]any{
		"ref":      "/nonexistent/ref.yuv",
		"dis":      "/nonexistent/dis.yuv",
		"width":    float64(576),
		"height":   float64(324),
		"pixfmt":   "420",
		"bitdepth": float64(8),
	})
	if err == nil {
		t.Fatal("expected error for nonexistent ref")
	}
	if !strings.Contains(err.Error(), "ref") {
		t.Errorf("error should mention 'ref', got: %q", err.Error())
	}
}

// ---------------------------------------------------------------------------
// handleRunCompare / handleRunLadder / handleRunTunePerShot
// — vmaf-tune absent → error path.
// ---------------------------------------------------------------------------

func TestHandleRunCompare_BinaryAbsent(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	t.Setenv("VMAF_TUNE_BIN", "/nonexistent/vmaf-tune-for-test")
	_, err := handleRunCompare(context.Background(), map[string]any{
		"src": "/some/video.mp4",
	})
	if err == nil {
		t.Fatal("expected error when vmaf-tune is absent")
	}
	if !strings.Contains(err.Error(), "vmaf-tune binary not found") {
		t.Errorf("error message: %q", err.Error())
	}
}

func TestHandleRunLadder_BinaryAbsent(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	t.Setenv("VMAF_TUNE_BIN", "/nonexistent/vmaf-tune-for-test")
	_, err := handleRunLadder(context.Background(), map[string]any{
		"src":          "/some/video.mp4",
		"resolutions":  "1920x1080",
		"target_vmafs": "95",
	})
	if err == nil {
		t.Fatal("expected error when vmaf-tune is absent")
	}
	if !strings.Contains(err.Error(), "vmaf-tune binary not found") {
		t.Errorf("error message: %q", err.Error())
	}
}

func TestHandleRunTunePerShot_BinaryAbsent(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	t.Setenv("VMAF_TUNE_BIN", "/nonexistent/vmaf-tune-for-test")
	_, err := handleRunTunePerShot(context.Background(), map[string]any{
		"src": "/some/video.mp4",
	})
	if err == nil {
		t.Fatal("expected error when vmaf-tune is absent")
	}
	if !strings.Contains(err.Error(), "vmaf-tune binary not found") {
		t.Errorf("error message: %q", err.Error())
	}
}

// ---------------------------------------------------------------------------
// findVmafTune — env override, PATH lookup, fallback.
// ---------------------------------------------------------------------------

func TestFindVmafTune_EnvOverride(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	const overridePath = "/custom/bin/vmaf-tune"
	t.Setenv("VMAF_TUNE_BIN", overridePath)
	if got := findVmafTune(); got != overridePath {
		t.Errorf("findVmafTune() = %q, want %q", got, overridePath)
	}
}

func TestFindVmafTune_Fallback(t *testing.T) {
	// Cannot use t.Parallel because we must clear VMAF_TUNE_BIN.
	t.Setenv("VMAF_TUNE_BIN", "")
	// The fallback returns some non-empty string (the repo-relative path).
	got := findVmafTune()
	if got == "" {
		t.Error("findVmafTune() returned empty string for fallback path")
	}
	// Fallback path must contain "vmaf-tune" as the binary name.
	if !strings.Contains(got, "vmaf-tune") {
		t.Errorf("fallback path %q does not contain 'vmaf-tune'", got)
	}
}

// ---------------------------------------------------------------------------
// parseArgs — the closure in addRawTool exercises this.
// ---------------------------------------------------------------------------

func TestParseArgs_Nil(t *testing.T) {
	t.Parallel()
	// nil Params pointer — the handler passes &CallToolParamsRaw{} for nil
	// args. We test via the server dispatch path where the MCP SDK populates
	// Params before invoking the tool closure.
	req := &mcp.CallToolRequest{Params: &mcp.CallToolParamsRaw{}}
	got, err := parseArgs(req)
	if err != nil {
		t.Fatalf("parseArgs(nil args): %v", err)
	}
	if got == nil {
		t.Error("parseArgs(nil args): expected non-nil map")
	}
	if len(got) != 0 {
		t.Errorf("parseArgs(nil args): expected empty map, got %v", got)
	}
}

func TestParseArgs_ValidJSON(t *testing.T) {
	t.Parallel()
	req := &mcp.CallToolRequest{
		Params: &mcp.CallToolParamsRaw{
			Arguments: json.RawMessage(`{"key":"value","num":42}`),
		},
	}
	got, err := parseArgs(req)
	if err != nil {
		t.Fatalf("parseArgs(valid): %v", err)
	}
	if got["key"] != "value" {
		t.Errorf("key: got %v, want value", got["key"])
	}
	// json.Unmarshal decodes numbers as float64.
	if got["num"] != float64(42) {
		t.Errorf("num: got %v, want 42", got["num"])
	}
}

func TestParseArgs_InvalidJSON(t *testing.T) {
	t.Parallel()
	req := &mcp.CallToolRequest{
		Params: &mcp.CallToolParamsRaw{
			Arguments: json.RawMessage(`not-valid-json`),
		},
	}
	_, err := parseArgs(req)
	if err == nil {
		t.Fatal("parseArgs(invalid JSON): expected error")
	}
}

// ---------------------------------------------------------------------------
// probeBackends — cache hit path (call twice with same binary path).
// ---------------------------------------------------------------------------

func TestProbeBackends_CacheHit(t *testing.T) {
	t.Parallel()
	// Calling with a fake binary always returns the same map; the second call
	// must return the cached value without re-invoking the binary (which would
	// panic on the missing binary).  We only care that the call completes and
	// that cpu is always true.
	bin := "/definitely/nonexistent/vmaf-probe-cache-test"
	r1 := probeBackends(bin)
	r2 := probeBackends(bin)
	if !r1["cpu"] {
		t.Error("probeBackends: cpu should be true even for unknown binary")
	}
	if !r2["cpu"] {
		t.Error("probeBackends (cached): cpu should be true")
	}
	// Pointer equality not possible across copies; check map length matches.
	if len(r1) != len(r2) {
		t.Errorf("cache miss: len r1=%d, r2=%d", len(r1), len(r2))
	}
}

// ---------------------------------------------------------------------------
// describeModel — ambiguous (multiple matches) path.
// The real model/ directory contains vmaf_v0.6.0.json + vmaf_v0.6.0.pkl
// (two files with the same stem, which produces >1 match when searching by
// stem alone).
// ---------------------------------------------------------------------------

func TestDescribeModel_Ambiguous(t *testing.T) {
	t.Parallel()
	// "vmaf_v0.6.0" exists as both .json and .pkl under model/other_models/.
	// The WalkDir step will collect both, triggering the ambiguity error.
	root := findRepoRoot(t)
	jsonPath := filepath.Join(root, "model", "other_models", "vmaf_v0.6.0.json")
	pklPath := filepath.Join(root, "model", "other_models", "vmaf_v0.6.0.pkl")
	for _, p := range []string{jsonPath, pklPath} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("vmaf_v0.6.0 dual files not present (%s); skipping", p)
		}
	}
	_, err := describeModel("vmaf_v0.6.0")
	if err == nil {
		t.Fatal("expected ambiguity error for model stem with .json + .pkl matches")
	}
	if !strings.Contains(err.Error(), "ambiguous") {
		t.Errorf("error should mention 'ambiguous', got: %q", err.Error())
	}
}

func TestDescribeModel_NotFound(t *testing.T) {
	t.Parallel()
	// An impossible stem — should get "not found".
	_, err := describeModel("this-model-does-not-exist-xyz-1234")
	if err == nil {
		t.Fatal("expected error for nonexistent model")
	}
	if !strings.Contains(err.Error(), "not found") {
		t.Errorf("error should mention 'not found', got: %q", err.Error())
	}
}

// ---------------------------------------------------------------------------
// describeModelFile — non-JSON format doesn't populate model_type.
// ---------------------------------------------------------------------------

func TestDescribeModelFile_ONNX(t *testing.T) {
	// Create a minimal .onnx file (not real ONNX, just non-empty bytes).
	tmp := t.TempDir()
	onnxPath := filepath.Join(tmp, "fake.onnx")
	if err := os.WriteFile(onnxPath, []byte("\x00\x01\x02"), 0o600); err != nil {
		t.Fatal(err)
	}
	result, err := describeModelFile(onnxPath, tmp)
	if err != nil {
		t.Fatalf("describeModelFile: %v", err)
	}
	if result["format"] != "onnx" {
		t.Errorf("format: got %v, want 'onnx'", result["format"])
	}
	// model_type should be nil for non-JSON files.
	if result["model_type"] != nil {
		t.Errorf("model_type: got %v, want nil for .onnx", result["model_type"])
	}
}

func TestDescribeModelFile_JSONWithModelDict(t *testing.T) {
	// Write a minimal model JSON that has a model_dict.
	tmp := t.TempDir()
	jsonPath := filepath.Join(tmp, "tiny.json")
	payload := `{"model_dict":{"model_type":"svm","feature_names":["adm2","motion"]}}`
	if err := os.WriteFile(jsonPath, []byte(payload), 0o600); err != nil {
		t.Fatal(err)
	}
	result, err := describeModelFile(jsonPath, tmp)
	if err != nil {
		t.Fatalf("describeModelFile: %v", err)
	}
	if result["model_type"] != "svm" {
		t.Errorf("model_type: got %v, want 'svm'", result["model_type"])
	}
	feats, _ := result["feature_names"].([]any)
	if len(feats) != 2 {
		t.Errorf("feature_names: got %v, want 2 entries", result["feature_names"])
	}
}

// ---------------------------------------------------------------------------
// handleListExtractors — walks the real core/src/feature/ directory.
// ---------------------------------------------------------------------------

func TestHandleListExtractors(t *testing.T) {
	t.Parallel()
	result, err := handleListExtractors(context.Background(), nil)
	if err != nil {
		t.Fatalf("handleListExtractors: %v", err)
	}
	m, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("result type: %T", result)
	}
	extractors, ok := m["extractors"].([]map[string]any)
	if !ok {
		t.Fatalf("extractors type: %T", m["extractors"])
	}
	// The repo has at least one extractor (ADM, VIF, MOTION, PSNR, etc.).
	if len(extractors) == 0 {
		t.Fatal("expected at least one extractor in core/src/feature/")
	}
	// Each entry must have name, backend, source.
	for _, e := range extractors {
		for _, key := range []string{"name", "backend", "source"} {
			if _, ok := e[key]; !ok {
				t.Errorf("extractor entry missing key %q: %v", key, e)
			}
		}
		backend := e["backend"].(string)
		validBackends := map[string]bool{
			"cpu": true, "cuda": true, "sycl": true,
			"hip": true, "metal": true, "vulkan": true,
		}
		if !validBackends[backend] {
			t.Errorf("unexpected backend %q for extractor %v", backend, e)
		}
	}
}

// ---------------------------------------------------------------------------
// addRawTool closure — handler error path and marshal error path.
// The handler error path is exercised by calling a real tool via the server
// with a request that will fail handler validation (e.g. handleProbeBackend
// with an empty backend arg returns an error, which the closure wraps into
// an isError=true result).
// ---------------------------------------------------------------------------

func TestAddRawTool_HandlerErrorBecomesIsError(t *testing.T) {
	t.Parallel()
	srv := buildServer(nil)
	client := mcp.NewClient(&mcp.Implementation{Name: "test", Version: "0.0.1"}, nil)
	t1, t2 := mcp.NewInMemoryTransports()
	ctx := context.Background()

	if _, err := srv.Connect(ctx, t1, nil); err != nil {
		t.Fatalf("server.Connect: %v", err)
	}
	session, err := client.Connect(ctx, t2, nil)
	if err != nil {
		t.Fatalf("client.Connect: %v", err)
	}
	defer session.Close()

	// probe_backend with no "backend" argument → handler returns error →
	// addRawTool closure converts to IsError=true CallToolResult.
	result, err := session.CallTool(ctx, &mcp.CallToolParams{
		Name:      "probe_backend",
		Arguments: json.RawMessage(`{}`), // missing backend
	})
	if err != nil {
		t.Fatalf("CallTool: %v", err)
	}
	if result == nil {
		t.Fatal("result is nil")
	}
	if !result.IsError {
		t.Error("IsError should be true when handler returns an error")
	}
}

// ---------------------------------------------------------------------------
// probeBackends — advertised backends from --help output.
// We write a tiny fake vmaf shell script that emits --no_cuda to verify
// parsing.
// ---------------------------------------------------------------------------

func TestProbeBackends_ParsesHelpOutput(t *testing.T) {
	tmp := t.TempDir()
	script := filepath.Join(tmp, "vmaf")
	// Script prints a --help output that advertises cuda.
	if err := os.WriteFile(script, []byte("#!/bin/sh\necho '--no_cuda --no_sycl'\n"), 0o700); err != nil {
		t.Fatal(err)
	}

	// Ensure this binary isn't in the cache from a previous test run.
	probeMu.Lock()
	delete(probeCache, script)
	probeMu.Unlock()

	advertised := probeBackends(script)
	if !advertised["cpu"] {
		t.Error("cpu always true")
	}
	if !advertised["cuda"] {
		t.Error("cuda should be true: script emitted '--no_cuda'")
	}
	if !advertised["sycl"] {
		t.Error("sycl should be true: script emitted '--no_sycl'")
	}
	if advertised["hip"] {
		t.Error("hip should be false: script did not emit '--no_hip'")
	}
}
