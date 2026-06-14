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
	"strings"
	"testing"

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

// TestScoreExtraPropertiesPresent asserts the new ADR-1117 parameters are
// advertised on both score tools and that existing required fields are intact.
func TestScoreExtraPropertiesPresent(t *testing.T) {
	t.Parallel()
	want := []string{
		"feature", "aom_ctc", "nflx_ctc",
		"tiny_model", "tiny_device", "tiny_threads", "tiny_fp16",
		"tiny_model_verify", "tiny_codec", "tiny_preset", "tiny_crf",
		"tiny_resize", "no_reference",
		"threads", "frame_cnt", "frame_skip_ref", "frame_skip_dist", "no_prediction",
	}
	for _, tool := range []string{"vmaf_score", "vmaf_score_encoded"} {
		props := scoreToolProperties(t, tool)
		for _, key := range want {
			if _, ok := props[key]; !ok {
				t.Errorf("tool %q: missing ADR-1117 property %q", tool, key)
			}
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
		"tiny_resize": {"bilinear", "nearest", "bicubic", "disabled"},
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
	}
	argv := parseScoreExtras(args).appendArgs(nil)
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
	}
	for _, sub := range wantSubstrings {
		if !strings.Contains(joined, sub) {
			t.Errorf("argv %q missing %q", joined, sub)
		}
	}
}

// TestSubsampleForwarded pins the fix for the pre-existing Go/Python parity
// bug where vmaf_score_encoded declared `subsample` but never forwarded it to
// the CLI. Python emits `--subsample N` only when N > 1, ahead of the other
// extras (server.py:755).
func TestSubsampleForwarded(t *testing.T) {
	t.Parallel()
	// N > 1 is emitted, positioned before --feature (matches server.py order).
	argv := parseScoreExtras(map[string]any{
		"subsample": float64(5),
		"feature":   []any{"psnr"},
	}).appendArgs(nil)
	joined := strings.Join(argv, " ")
	if !strings.Contains(joined, "--subsample 5") {
		t.Errorf("argv %q missing %q", joined, "--subsample 5")
	}
	if i, j := strings.Index(joined, "--subsample"), strings.Index(joined, "--feature"); i < 0 || j < 0 || i > j {
		t.Errorf("argv %q: --subsample must precede --feature (Python-parity order)", joined)
	}
	// N <= 1 emits nothing and leaves the request cgo-direct-path eligible.
	ex := parseScoreExtras(map[string]any{"subsample": float64(1)})
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
	ex := parseScoreExtras(map[string]any{
		"ref": "/a.yuv", "dis": "/b.yuv", "width": float64(64), "height": float64(64),
	})
	if !ex.isZero() {
		t.Errorf("parseScoreExtras with no extras: isZero()=false, want true")
	}
	if argv := ex.appendArgs(nil); len(argv) != 0 {
		t.Errorf("empty extras appended %d flags: %v", len(argv), argv)
	}
}

// TestOptIntDistinguishesZeroFromUnset guards the optIntArg helper: an explicit
// 0 must be forwarded (e.g. --frame_skip_dist 0), an absent key must not be.
func TestOptIntDistinguishesZeroFromUnset(t *testing.T) {
	t.Parallel()
	if p := optIntArg(map[string]any{"frame_skip_dist": float64(0)}, "frame_skip_dist"); p == nil || *p != 0 {
		t.Errorf("explicit 0 should yield pointer to 0, got %v", p)
	}
	if p := optIntArg(map[string]any{}, "frame_skip_dist"); p != nil {
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
