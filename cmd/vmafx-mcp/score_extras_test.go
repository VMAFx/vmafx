// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// score_extras_test.go covers the ADR-1117 optional scoring pass-through
// parameters added to vmaf_score / vmaf_score_encoded: that the tool schemas
// advertise them, that parseScoreExtras maps MCP args onto the correct vmaf
// CLI flags, and that a no-arg request stays byte-compatible (no extra flags).

package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// scoreToolProperties returns the JSON-decoded "properties" object of the named
// tool's input schema, via the live in-memory MCP server.
func scoreToolProperties(t *testing.T, toolName string) map[string]any {
	t.Helper()
	srv := buildServer(nil)
	client := mcp.NewClient(&mcp.Implementation{Name: "test-client", Version: "0.0.1"}, nil)
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
	result, err := session.ListTools(ctx, &mcp.ListToolsParams{})
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}
	for _, tool := range result.Tools {
		if tool.Name != toolName {
			continue
		}
		b, err := json.Marshal(tool.InputSchema)
		if err != nil {
			t.Fatalf("marshal schema: %v", err)
		}
		var schema map[string]any
		if err := json.Unmarshal(b, &schema); err != nil {
			t.Fatalf("unmarshal schema: %v", err)
		}
		props, _ := schema["properties"].(map[string]any)
		if props == nil {
			t.Fatalf("tool %q has no properties", toolName)
		}
		return props
	}
	t.Fatalf("tool %q not found", toolName)
	return nil
}

// TestScoreExtraPropertiesPresent asserts the new ADR-1117 and #1240 parameters are
// advertised on both score tools and that existing required fields are intact.
func TestScoreExtraPropertiesPresent(t *testing.T) {
	t.Parallel()
	want := []string{
		"feature", "aom_ctc", "nflx_ctc",
		"tiny_model", "tiny_device", "dnn_ep", "tiny_threads", "tiny_fp16",
		"tiny_model_verify", "tiny_codec", "tiny_preset", "tiny_crf",
		"tiny_resize", "no_reference",
		"threads", "frame_cnt", "frame_skip_ref", "frame_skip_dist", "no_prediction",
		"cpumask", "gpumask", "sycl_device", "hip_device", "metal_device",
		"output_fmt",
	}
	for _, tool := range []string{"vmaf_score", "vmaf_score_encoded"} {
		props := scoreToolProperties(t, tool)
		for _, key := range want {
			if _, ok := props[key]; !ok {
				t.Errorf("tool %q: missing property %q", tool, key)
			}
		}
		// Subsample is on both tools.
		if _, ok := props["subsample"]; !ok {
			t.Errorf("tool %q: missing subsample property", tool)
		}
		// Backward-compat: the original required-by-default fields still present.
		for _, key := range []string{"model", "backend", "precision"} {
			if _, ok := props[key]; !ok {
				t.Errorf("tool %q: lost pre-existing property %q", tool, key)
			}
		}
	}
}

// TestScoreExtraEnums spot-checks the constrained enums match cli_parse.c.
func TestScoreExtraEnums(t *testing.T) {
	t.Parallel()
	props := scoreToolProperties(t, "vmaf_score")
	cases := map[string][]string{
		"aom_ctc":     {"v1.0", "v2.0", "v3.0", "v4.0", "v5.0", "v6.0", "v7.0"},
		"nflx_ctc":    {"v1.0"},
		"tiny_device": {"auto", "cpu", "cuda", "openvino", "openvino-npu", "openvino-cpu", "openvino-gpu", "coreml", "coreml-ane", "coreml-gpu", "coreml-cpu", "rocm"},
		"dnn_ep":      {"auto", "cpu", "cuda", "openvino", "openvino-npu", "openvino-cpu", "openvino-gpu", "coreml", "coreml-ane", "coreml-gpu", "coreml-cpu", "rocm"},
		"tiny_resize": {"bilinear", "nearest", "bicubic", "disabled"},
		"output_fmt":  {"json", "xml", "csv", "sub"},
	}
	for key, wantEnum := range cases {
		prop, _ := props[key].(map[string]any)
		rawEnum, _ := prop["enum"].([]any)
		got := make([]string, 0, len(rawEnum))
		for _, e := range rawEnum {
			got = append(got, e.(string))
		}
		if strings.Join(got, ",") != strings.Join(wantEnum, ",") {
			t.Errorf("property %q enum = %v, want %v", key, got, wantEnum)
		}
	}
}

// TestParseScoreExtrasMapsFlags verifies each MCP param maps to the right CLI
// flag in the right order. This is the core "args map correctly" guard.
func TestParseScoreExtrasMapsFlags(t *testing.T) {
	t.Parallel()
	args := map[string]any{
		"feature":           []any{"psnr", "cambi=full_ref=true"},
		"aom_ctc":           "v3.0",
		"tiny_model":        "/m/nr.onnx",
		"tiny_device":       "cuda",
		"tiny_threads":      float64(4),
		"tiny_fp16":         true,
		"tiny_model_verify": true,
		"tiny_codec":        "libx264",
		"tiny_preset":       "medium",
		"tiny_crf":          float64(23),
		"tiny_resize":       "bilinear",
		"no_reference":      true,
		"threads":           float64(8),
		"frame_cnt":         float64(100),
		"frame_skip_ref":    float64(2),
		"frame_skip_dist":   float64(0),
		"no_prediction":     true,
		"cpumask":           float64(3),
		"gpumask":           float64(1),
		"sycl_device":       float64(0),
		"hip_device":        float64(2),
		"metal_device":      float64(0),
		"output_fmt":        "csv",
	}
	ex, err := parseScoreExtras(args)
	if err != nil {
		t.Fatalf("parseScoreExtras: %v", err)
	}
	argv := ex.appendArgs(nil)
	joined := strings.Join(argv, " ")
	wantSubstrings := []string{
		"--feature psnr",
		"--feature cambi=full_ref=true",
		"--aom_ctc v3.0",
		"--tiny-model /m/nr.onnx",
		"--tiny-device cuda",
		"--tiny-threads 4",
		"--tiny-fp16",
		"--tiny-model-verify",
		"--tiny-codec libx264",
		"--tiny-preset medium",
		"--tiny-crf 23",
		"--tiny-resize bilinear",
		"--no-reference",
		"--threads 8",
		"--frame_cnt 100",
		"--frame_skip_ref 2",
		"--frame_skip_dist 0", // explicit 0 must be emitted (not omitted)
		"--no_prediction",
		"--cpumask 3",
		"--gpumask 1",
		"--sycl_device 0",
		"--hip_device 2",
		"--metal_device 0",
	}
	for _, sub := range wantSubstrings {
		if !strings.Contains(joined, sub) {
			t.Errorf("argv %q missing %q", joined, sub)
		}
	}
	if ex.outputFmt != "csv" {
		t.Errorf("outputFmt = %q, want 'csv'", ex.outputFmt)
	}
}

// TestSubsampleForwarded pins the fix for the pre-existing Go/Python parity
// bug where vmaf_score_encoded declared `subsample` but never forwarded it to
// the CLI. Python emits `--subsample N` only when N > 1, ahead of the other
// extras (server.py:755).
func TestSubsampleForwarded(t *testing.T) {
	t.Parallel()
	// N > 1 is emitted, positioned before --feature (matches server.py order).
	ex1, err := parseScoreExtras(map[string]any{
		"subsample": float64(5),
		"feature":   []any{"psnr"},
	})
	if err != nil {
		t.Fatalf("parseScoreExtras: %v", err)
	}
	argv := ex1.appendArgs(nil)
	joined := strings.Join(argv, " ")
	if !strings.Contains(joined, "--subsample 5") {
		t.Errorf("argv %q missing %q", joined, "--subsample 5")
	}
	if i, j := strings.Index(joined, "--subsample"), strings.Index(joined, "--feature"); i < 0 || j < 0 || i > j {
		t.Errorf("argv %q: --subsample must precede --feature (Python-parity order)", joined)
	}
	// N <= 1 emits nothing and leaves the request cgo-direct-path eligible.
	ex, err := parseScoreExtras(map[string]any{"subsample": float64(1)})
	if err != nil {
		t.Fatalf("parseScoreExtras: %v", err)
	}
	if !ex.isZero() {
		t.Errorf("subsample=1 alone: isZero()=false, want true")
	}
	if got := ex.appendArgs(nil); len(got) != 0 {
		t.Errorf("subsample=1 alone: argv=%v, want empty", got)
	}
}

// TestParseScoreExtrasEmpty confirms a request with no extra args emits no
// flags — the backward-compatibility guarantee.
func TestParseScoreExtrasEmpty(t *testing.T) {
	t.Parallel()
	ex, err := parseScoreExtras(map[string]any{
		"ref": "/a.yuv", "dis": "/b.yuv", "width": float64(64), "height": float64(64),
	})
	if err != nil {
		t.Fatalf("parseScoreExtras: %v", err)
	}
	if !ex.isZero() {
		t.Errorf("parseScoreExtras with no extras: isZero()=false, want true")
	}
	if argv := ex.appendArgs(nil); len(argv) != 0 {
		t.Errorf("empty extras appended %d flags: %v", len(argv), argv)
	}
}

// TestParseScoreExtrasValidation asserts that bad enums and negative values are rejected.
func TestParseScoreExtrasValidation(t *testing.T) {
	t.Parallel()
	badCases := []map[string]any{
		{"output_fmt": "yaml"},
		{"aom_ctc": "v99.0"},
		{"nflx_ctc": "v2.0"},
		{"tiny_device": "nonexistent"},
		{"tiny_resize": "lanczos"},
		{"subsample": float64(0)},
		{"threads": float64(0)},
		{"frame_cnt": float64(0)},
		{"frame_skip_ref": float64(-1)},
		{"frame_skip_dist": float64(-1)},
		{"tiny_threads": float64(-1)},
		{"tiny_crf": float64(-1)},
		{"tiny_crf": float64(64)},
		{"cpumask": float64(-1)},
		{"gpumask": float64(-1)},
		{"sycl_device": float64(-1)},
		{"hip_device": float64(-1)},
		{"metal_device": float64(-1)},
	}
	for _, tc := range badCases {
		if _, err := parseScoreExtras(tc); err == nil {
			t.Errorf("parseScoreExtras(%v) expected error, got nil", tc)
		}
	}
}

// TestOptIntDistinguishesZeroFromUnset guards the optIntArg helper: an explicit
// 0 must be forwarded (e.g. --frame_skip_dist 0), an absent key must not be.
func TestOptIntDistinguishesZeroFromUnset(t *testing.T) {
	t.Parallel()
	args := map[string]any{"frame_skip_dist": float64(0)}
	p := optIntArg(args, "frame_skip_dist")
	if p == nil || *p != 0 {
		t.Errorf("explicit 0 should yield &0, got %v", p)
	}
	if p := optIntArg(args, "threads"); p != nil {
		t.Errorf("absent key should yield nil, got %v", *p)
	}
}

// TestNoReferenceRequiresTinyModel pins the NR-mode gate: --no-reference without
// a tiny model is rejected with the same message the CLI emits (cli_parse.c:997).
func TestNoReferenceRequiresTinyModel(t *testing.T) {
	t.Parallel()
	_, err := handleVmafScore(context.Background(), map[string]any{
		"ref": "/nonexistent/a.yuv", "dis": "/nonexistent/b.yuv",
		"width": float64(64), "height": float64(64),
		"pixfmt": "420", "bitdepth": float64(8),
		"no_reference": true,
	})
	if err == nil || !strings.Contains(err.Error(), "no_reference requires tiny_model") {
		t.Errorf("expected no_reference-requires-tiny_model error, got %v", err)
	}
}

// TestScoreExtrasValidationRejectsBadEnums verifies that unknown enum values
// and out-of-range arguments are rejected before shelling out to the CLI.
func TestScoreExtrasValidationRejectsBadEnums(t *testing.T) {
	t.Parallel()

	testCases := []struct {
		name    string
		args    map[string]any
		wantErr string
	}{
		{
			name:    "invalid tiny_device",
			args:    map[string]any{"tiny_device": "invalid_dev"},
			wantErr: "invalid tiny_device",
		},
		{
			name:    "invalid dnn_ep",
			args:    map[string]any{"dnn_ep": "unknown_ep"},
			wantErr: "invalid tiny_device",
		},
		{
			name: "conflicting tiny_device and dnn_ep",
			args: map[string]any{
				"tiny_device": "cpu",
				"dnn_ep":      "cuda",
			},
			wantErr: "conflicting tiny_device",
		},
		{
			name:    "invalid tiny_resize",
			args:    map[string]any{"tiny_resize": "cubic"},
			wantErr: "invalid tiny_resize",
		},
		{
			name:    "tiny_crf negative",
			args:    map[string]any{"tiny_crf": float64(-1)},
			wantErr: "invalid tiny_crf",
		},
		{
			name:    "tiny_crf too large",
			args:    map[string]any{"tiny_crf": float64(64)},
			wantErr: "invalid tiny_crf",
		},
		{
			name:    "tiny_threads negative",
			args:    map[string]any{"tiny_threads": float64(-1)},
			wantErr: "invalid tiny_threads",
		},
		{
			name:    "invalid aom_ctc",
			args:    map[string]any{"aom_ctc": "v8.0"},
			wantErr: "invalid aom_ctc",
		},
		{
			name:    "invalid nflx_ctc",
			args:    map[string]any{"nflx_ctc": "v2.0"},
			wantErr: "invalid nflx_ctc",
		},
		{
			name:    "threads zero",
			args:    map[string]any{"threads": float64(0)},
			wantErr: "invalid threads",
		},
		{
			name:    "frame_cnt zero",
			args:    map[string]any{"frame_cnt": float64(0)},
			wantErr: "invalid frame_cnt",
		},
		{
			name:    "frame_skip_ref negative",
			args:    map[string]any{"frame_skip_ref": float64(-1)},
			wantErr: "invalid frame_skip_ref",
		},
		{
			name:    "subsample zero",
			args:    map[string]any{"subsample": float64(0)},
			wantErr: "invalid subsample",
		},
	}

	for _, tc := range testCases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := parseScoreExtras(tc.args)
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("expected error containing %q, got %v", tc.wantErr, err)
			}
		})
	}
}

// TestDnnEPAliasSupported verifies dnn_ep alone sets the tiny-device flag.
func TestDnnEPAliasSupported(t *testing.T) {
	t.Parallel()
	ex, err := parseScoreExtras(map[string]any{
		"dnn_ep": "openvino-npu",
	})
	if err != nil {
		t.Fatalf("parseScoreExtras: %v", err)
	}
	argv := ex.appendArgs(nil)
	joined := strings.Join(argv, " ")
	if !strings.Contains(joined, "--tiny-device openvino-npu") {
		t.Errorf("argv %q missing '--tiny-device openvino-npu'", joined)
	}
}

// TestVmafScoreRejectsInvalidCoreParams verifies pixfmt, bitdepth, and backend validation.
func TestVmafScoreRejectsInvalidCoreParams(t *testing.T) {
	t.Parallel()

	repoRoot := findRepoRoot(t)
	dummyPath := repoRoot + "/model/vmaf_v0.6.1.json"

	cases := []struct {
		name    string
		args    map[string]any
		wantErr string
	}{
		{
			name: "invalid pixfmt",
			args: map[string]any{
				"ref": dummyPath, "dis": dummyPath,
				"width": float64(64), "height": float64(64),
				"pixfmt": "422p", "bitdepth": float64(8),
			},
			wantErr: "invalid pixfmt",
		},
		{
			name: "invalid bitdepth",
			args: map[string]any{
				"ref": dummyPath, "dis": dummyPath,
				"width": float64(64), "height": float64(64),
				"pixfmt": "420", "bitdepth": float64(14),
			},
			wantErr: "invalid bitdepth",
		},
		{
			name: "invalid backend",
			args: map[string]any{
				"ref": dummyPath, "dis": dummyPath,
				"width": float64(64), "height": float64(64),
				"pixfmt": "420", "bitdepth": float64(8),
				"backend": "vulkan",
			},
			wantErr: "invalid backend",
		},
	}

	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := handleVmafScore(context.Background(), tc.args)
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("expected error containing %q, got %v", tc.wantErr, err)
			}
		})
	}
}

// TestE2EScoreCPUWithTinyAIFlagsAndErrorPath runs the vmaf_score tool against the
// real host vmaf CLI on the Netflix src01 pair with scoring extra flags, and
// verifies the missing-model error path returns an error.
func TestE2EScoreCPUWithTinyAIFlagsAndErrorPath(t *testing.T) {
	repoRoot := findRepoRoot(t)
	ref := filepath.Join(repoRoot, "python", "test", "resource", "yuv", "src01_hrc00_576x324.yuv")
	dis := filepath.Join(repoRoot, "python", "test", "resource", "yuv", "src01_hrc01_576x324.yuv")
	if _, err := os.Stat(ref); err != nil {
		t.Skipf("Netflix fixture ref missing: %v", err)
	}
	if _, err := os.Stat(dis); err != nil {
		t.Skipf("Netflix fixture dis missing: %v", err)
	}
	vmafBin := libvmaf.FindBinary()
	if _, err := os.Stat(vmafBin); err != nil {
		t.Skipf("vmaf binary %s not found: %v", vmafBin, err)
	}

	realRef, err := filepath.EvalSymlinks(ref)
	if err == nil {
		t.Setenv("VMAF_MCP_ALLOW", filepath.Dir(realRef))
	} else {
		t.Setenv("VMAF_MCP_ALLOW", filepath.Dir(ref))
	}

	// 1. Normal CPU scoring with extra scoring flags passed through
	res, err := handleVmafScore(context.Background(), map[string]any{
		"ref":       ref,
		"dis":       dis,
		"width":     float64(576),
		"height":    float64(324),
		"pixfmt":    "420",
		"bitdepth":  float64(8),
		"model":     "version=vmaf_v0.6.1",
		"frame_cnt": float64(3),
		"threads":   float64(2),
		"subsample": float64(1),
	})
	if err != nil {
		t.Fatalf("handleVmafScore failed: %v", err)
	}
	resMap, ok := res.(map[string]any)
	if !ok {
		t.Fatalf("expected map[string]any result, got %T", res)
	}
	if _, ok := resMap["pooled_metrics"]; !ok {
		t.Errorf("missing pooled_metrics in response: %v", resMap)
	}

	// 2. Missing model error path returns error (which addRawTool maps to isError=True)
	_, err = handleVmafScore(context.Background(), map[string]any{
		"ref":      ref,
		"dis":      dis,
		"width":    float64(576),
		"height":   float64(324),
		"pixfmt":   "420",
		"bitdepth": float64(8),
		"model":    "path=/nonexistent/missing_model_path.json",
	})
	if err == nil {
		t.Fatal("expected error on missing model path, got nil")
	}
}
