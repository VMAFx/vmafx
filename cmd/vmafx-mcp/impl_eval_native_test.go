// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// impl_eval_native_test.go — end-to-end coverage for the native
// eval_model_on_split / compare_models path that replaced the python3
// shell-out (ADR-0704 Stage 2).
//
// These tests drive the real handlers against the committed parquet
// fixture and a committed .onnx model, so they exercise the whole chain:
// ValidatePath -> split validation -> parquet read -> ONNX session open.
// The final step depends on whether libvmaf was built with ONNX Runtime,
// so both outcomes are accepted; what is asserted is that the handler
// reaches that step natively and never invokes python3.

package main

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/modeleval"
)

// repoFile resolves a repo-relative path, skipping if it is absent.
func repoFile(t *testing.T, rel string) string {
	t.Helper()
	p := filepath.Join(findRepoRoot(t), rel)
	if _, err := os.Stat(p); err != nil {
		t.Skipf("%s not present: %v", rel, err)
	}
	return p
}

// fixtureParquet returns the committed feature cache and extends the
// MCP path allowlist to cover it. AllowedRoots() only allowlists
// testdata/, python/test/resource/ and model/, and the parquet fixture
// lives under ai/testdata/. t.Setenv makes this test non-parallel.
func fixtureParquet(t *testing.T) string {
	t.Helper()
	p := repoFile(t, filepath.Join("ai", "testdata", "bisect", "features.parquet"))
	t.Setenv("VMAF_MCP_ALLOW", filepath.Dir(p))
	return p
}

func fixtureModel(t *testing.T) string {
	t.Helper()
	for _, rel := range []string{
		filepath.Join("model", "tiny", "fr_regressor_v1.onnx"),
		filepath.Join("model", "predictor_libx264.onnx"),
	} {
		p := filepath.Join(findRepoRoot(t), rel)
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	t.Skip("no committed .onnx model found")
	return ""
}

// TestHandleEvalModelOnSplit_NativePath proves the handler runs the
// native chain. Either it produces a Result (ORT present) or it fails
// with the DNN-unavailable error (ORT absent) — never with a Python
// import or "python3 not found" error.
func TestHandleEvalModelOnSplit_NativePath(t *testing.T) {
	res, err := handleEvalModelOnSplit(context.Background(), map[string]any{
		"model":    fixtureModel(t),
		"features": fixtureParquet(t),
		"split":    modeleval.SplitAll,
	})
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "python") ||
			strings.Contains(err.Error(), "onnxruntime, pandas, scipy") {
			t.Fatalf("handler still delegates to Python: %v", err)
		}
		if !libvmaf.DNNAvailable() && !errors.Is(err, libvmaf.ErrDNNUnavailable) {
			t.Fatalf("expected ErrDNNUnavailable on a build without ORT, got %v", err)
		}
		t.Logf("native path reached the ONNX step and reported: %v", err)
		return
	}
	out, ok := res.(*modeleval.Result)
	if !ok {
		t.Fatalf("result type = %T, want *modeleval.Result", res)
	}
	if out.N != 256 {
		t.Errorf("n = %d, want 256", out.N)
	}
	if len(out.Columns) == 0 {
		t.Error("columns is empty")
	}
}

// TestHandleEvalModelOnSplit_RejectsBadSplit checks the split is
// validated before any session is opened, so a typo is cheap.
func TestHandleEvalModelOnSplit_RejectsBadSplit(t *testing.T) {
	_, err := handleEvalModelOnSplit(context.Background(), map[string]any{
		"model":    fixtureModel(t),
		"features": fixtureParquet(t),
		"split":    "not-a-split",
	})
	if err == nil {
		t.Fatal("expected an error for an invalid split")
	}
	if !strings.Contains(err.Error(), "split must be one of") {
		t.Errorf("error = %q, want the split-validation message", err)
	}
}

// TestHandleEvalModelOnSplit_MissingFeatures covers the features-side
// ValidatePath rejection (the model-side one is covered elsewhere).
func TestHandleEvalModelOnSplit_MissingFeatures(t *testing.T) {
	_, err := handleEvalModelOnSplit(context.Background(), map[string]any{
		"model":    fixtureModel(t),
		"features": "/nonexistent/features.parquet",
	})
	if err == nil {
		t.Fatal("expected an error for a missing features path")
	}
	if !strings.Contains(err.Error(), "features") {
		t.Errorf("error = %q, want it to mention 'features'", err)
	}
}

// TestHandleCompareModels_NativePath drives the multi-model path and
// asserts every model is accounted for in exactly one of the two lists.
func TestHandleCompareModels_NativePath(t *testing.T) {
	model := fixtureModel(t)
	res, err := handleCompareModels(context.Background(), map[string]any{
		"models":   []any{model, "/nope/missing.onnx"},
		"features": fixtureParquet(t),
		"split":    modeleval.SplitAll,
	})
	if err != nil {
		t.Fatalf("unexpected handler error: %v", err)
	}
	cmp, ok := res.(*modeleval.Comparison)
	if !ok {
		t.Fatalf("result type = %T, want *modeleval.Comparison", res)
	}
	if got := len(cmp.Ranked) + len(cmp.Errors); got != 2 {
		t.Errorf("ranked+errors = %d, want 2 (%+v / %+v)", got, cmp.Ranked, cmp.Errors)
	}
	// The bogus path must always land in errors.
	found := false
	for _, e := range cmp.Errors {
		if strings.Contains(e.Model, "missing.onnx") {
			found = true
		}
		if strings.Contains(strings.ToLower(e.Error), "python") {
			t.Errorf("error mentions Python: %+v", e)
		}
	}
	if !found {
		t.Errorf("bogus model not reported in errors: %+v", cmp.Errors)
	}
}

// TestCompareModelsRejectsEmptyList pins the argument guard.
func TestCompareModelsRejectsEmptyList(t *testing.T) {
	cases := []struct {
		name   string
		models any
	}{
		{"empty list", []any{}},
		{"wrong type", "not-a-list"},
		{"nil", nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := handleCompareModels(context.Background(), map[string]any{
				"models":   tc.models,
				"features": fixtureParquet(t),
			})
			if err == nil {
				t.Fatal("expected an error")
			}
			if !strings.Contains(err.Error(), "non-empty list") {
				t.Errorf("error = %q, want the non-empty-list message", err)
			}
		})
	}
}

// TestNoPythonShellOutRemains is a guard against regressing to the old
// design: the eval tools must not reference a python3 helper any more.
func TestNoPythonShellOutRemains(t *testing.T) {
	t.Parallel()
	src, err := os.ReadFile("impl.go")
	if err != nil {
		t.Fatalf("read impl.go: %v", err)
	}
	for _, banned := range []string{"delegateToPythonEval", "import onnxruntime", "onnxruntime, pandas, scipy"} {
		if strings.Contains(string(src), banned) {
			t.Errorf("impl.go still contains %q — the Python shell-out is back", banned)
		}
	}
}
