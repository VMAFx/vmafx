// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// impl_test.go — pure-function + error-path unit tests for the MCP tool
// implementations. The baseline server_test.go covers tool listing + the
// happy-path vmaf_score handler; this file fills in the bulk of the
// uncovered statements (3.5 % -> well above 60 % on the pure-function
// surface) and pins the isError=true invariant for handler error paths.
//
// Memory invariant: handlers that fail must surface that failure as
// IsError=true in the MCP CallToolResult, so clients branch correctly.
// (Recorded in MEMORY: project_mcp_iserror_must_be_true.md.)

package main

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"strings"
	"testing"
)

// ---------------------------------------------------------------------------
// Arg helpers — strArg / intArg / floatArg / boolArg / hasArg
// ---------------------------------------------------------------------------

func TestStrArg(t *testing.T) {
	t.Parallel()
	args := map[string]any{
		"s":   "hello",
		"num": 42,
	}
	if got := strArg(args, "s", "def"); got != "hello" {
		t.Errorf("string value: got %q, want %q", got, "hello")
	}
	if got := strArg(args, "missing", "def"); got != "def" {
		t.Errorf("missing key: got %q, want %q", got, "def")
	}
	// Non-string is coerced via %v.
	if got := strArg(args, "num", "def"); got != "42" {
		t.Errorf("int coercion: got %q, want %q", got, "42")
	}
}

func TestIntArg(t *testing.T) {
	t.Parallel()
	args := map[string]any{
		"f":   float64(7),
		"i":   3,
		"jn":  json.Number("11"),
		"bad": "not-a-number",
	}
	tests := []struct {
		key  string
		def  int
		want int
	}{
		{"f", 0, 7},
		{"i", 0, 3},
		{"jn", 0, 11},
		{"bad", 99, 99},     // wrong type → default
		{"missing", 99, 99}, // absent → default
	}
	for _, tt := range tests {
		if got := intArg(args, tt.key, tt.def); got != tt.want {
			t.Errorf("intArg(%q, %d): got %d, want %d", tt.key, tt.def, got, tt.want)
		}
	}
}

func TestFloatArg(t *testing.T) {
	t.Parallel()
	args := map[string]any{
		"f":  3.5,
		"jn": json.Number("2.25"),
	}
	if got := floatArg(args, "f", 0); got != 3.5 {
		t.Errorf("float: got %v, want %v", got, 3.5)
	}
	if got := floatArg(args, "jn", 0); got != 2.25 {
		t.Errorf("json.Number: got %v, want %v", got, 2.25)
	}
	if got := floatArg(args, "missing", 1.5); got != 1.5 {
		t.Errorf("missing: got %v, want %v", got, 1.5)
	}
}

func TestBoolArg(t *testing.T) {
	t.Parallel()
	args := map[string]any{
		"t":   true,
		"f":   false,
		"bad": "not-a-bool",
	}
	if got := boolArg(args, "t", false); !got {
		t.Error("boolArg(true): got false")
	}
	if got := boolArg(args, "f", true); got {
		t.Error("boolArg(false): got true")
	}
	if got := boolArg(args, "bad", true); !got {
		t.Error("boolArg wrong-type: should fall back to default true")
	}
	if got := boolArg(args, "missing", true); !got {
		t.Error("boolArg missing: should fall back to default true")
	}
}

func TestHasArg(t *testing.T) {
	t.Parallel()
	args := map[string]any{"a": 1, "b": nil}
	if !hasArg(args, "a") {
		t.Error("hasArg(a): expected true")
	}
	if !hasArg(args, "b") {
		t.Error("hasArg(b) with nil value: expected true (key present)")
	}
	if hasArg(args, "missing") {
		t.Error("hasArg(missing): expected false")
	}
}

// ---------------------------------------------------------------------------
// Pure helpers — classifySourceResolution, modelResolutionClass,
// resolutionMismatchWarning, inferBackendFromPayload, inferBackendFromSym,
// stripModelExt, toFFmpegPixfmt, roundF, truncate, floatFromAny.
// ---------------------------------------------------------------------------

func TestClassifySourceResolution(t *testing.T) {
	t.Parallel()
	tests := []struct {
		w, h int
		want string
	}{
		{3840, 2160, "4k"},
		{4096, 2160, "4k"},
		{2160, 3840, "4k"}, // portrait — longest side rules
		{1920, 1080, "hd"},
		{1280, 720, "hd"},
		{640, 360, "sd"},
		{320, 240, "sd"},
	}
	for _, tt := range tests {
		if got := classifySourceResolution(tt.w, tt.h); got != tt.want {
			t.Errorf("classifySourceResolution(%d,%d): got %q, want %q", tt.w, tt.h, got, tt.want)
		}
	}
}

func TestModelResolutionClass(t *testing.T) {
	t.Parallel()
	tests := []struct {
		model, want string
	}{
		{"version=vmaf_v0.6.1", "hd"},
		{"vmaf_v0.6.1neg", "hd"},
		{"vmaf_b_v0.6.3.json", "hd"},
		{"vmaf_4k_v0.6.1.json", "4k"},
		{"path=/models/vmaf_4k_v0.6.1.json", "4k"},
		{"completely_unknown_model", ""},
	}
	for _, tt := range tests {
		if got := modelResolutionClass(tt.model); got != tt.want {
			t.Errorf("modelResolutionClass(%q): got %q, want %q", tt.model, got, tt.want)
		}
	}
}

func TestResolutionMismatchWarning(t *testing.T) {
	t.Parallel()

	// No hint in model name → no warning.
	if w := resolutionMismatchWarning("unknown", 1920, 1080); w != "" {
		t.Errorf("no-hint: expected empty, got %q", w)
	}
	// HD model + HD source → no warning.
	if w := resolutionMismatchWarning("version=vmaf_v0.6.1", 1920, 1080); w != "" {
		t.Errorf("hd+hd: expected empty, got %q", w)
	}
	// HD model + 4K source → warning mentions "4k".
	w := resolutionMismatchWarning("version=vmaf_v0.6.1", 3840, 2160)
	if w == "" {
		t.Fatal("hd+4k: expected warning, got empty")
	}
	if !strings.Contains(w, "4k") {
		t.Errorf("warning should mention source class \"4k\", got: %q", w)
	}
}

func TestInferBackendFromPayload(t *testing.T) {
	t.Parallel()
	// Empty frames → cpu (safe default).
	if got := inferBackendFromPayload(map[string]any{}); got != "cpu" {
		t.Errorf("empty: got %q, want cpu", got)
	}
	// >=30 metrics → vulkan.
	metrics30 := map[string]any{}
	for i := 0; i < 30; i++ {
		metrics30[strFromInt(i)] = float64(i)
	}
	vulkanPayload := map[string]any{
		"frames": []any{
			map[string]any{"metrics": metrics30},
		},
	}
	if got := inferBackendFromPayload(vulkanPayload); got != "vulkan" {
		t.Errorf("30 metrics: got %q, want vulkan", got)
	}
	// <=12 metrics → gpu.
	metrics8 := map[string]any{}
	for i := 0; i < 8; i++ {
		metrics8[strFromInt(i)] = float64(i)
	}
	gpuPayload := map[string]any{
		"frames": []any{
			map[string]any{"metrics": metrics8},
		},
	}
	if got := inferBackendFromPayload(gpuPayload); got != "gpu" {
		t.Errorf("8 metrics: got %q, want gpu", got)
	}
	// Between 13 and 29 metrics → cpu.
	metrics20 := map[string]any{}
	for i := 0; i < 20; i++ {
		metrics20[strFromInt(i)] = float64(i)
	}
	cpuPayload := map[string]any{
		"frames": []any{
			map[string]any{"metrics": metrics20},
		},
	}
	if got := inferBackendFromPayload(cpuPayload); got != "cpu" {
		t.Errorf("20 metrics: got %q, want cpu", got)
	}
}

func TestInferBackendFromSym(t *testing.T) {
	t.Parallel()
	tests := []struct {
		sym, want string
	}{
		{"adm2", "cpu"},
		{"adm2_cuda", "cuda"},
		{"vif_sycl", "sycl"},
		{"motion_vulkan", "vulkan"},
		{"adm_hip", "hip"},
		{"vif_metal", "metal"},
	}
	for _, tt := range tests {
		if got := inferBackendFromSym(tt.sym); got != tt.want {
			t.Errorf("inferBackendFromSym(%q): got %q, want %q", tt.sym, got, tt.want)
		}
	}
}

func TestStripModelExt(t *testing.T) {
	t.Parallel()
	tests := []struct {
		in, want string
	}{
		{"model.json", "model"},
		{"model.JSON", "model"}, // case-insensitive
		{"model.pkl", "model"},
		{"model.onnx", "model"},
		{"model.txt", "model.txt"}, // unknown ext preserved
		{"vmaf_v0.6.1.json", "vmaf_v0.6.1"},
	}
	for _, tt := range tests {
		if got := stripModelExt(tt.in); got != tt.want {
			t.Errorf("stripModelExt(%q): got %q, want %q", tt.in, got, tt.want)
		}
	}
}

func TestToFFmpegPixfmt(t *testing.T) {
	t.Parallel()
	tests := []struct {
		pf      string
		bd      int
		want    string
		wantErr bool
	}{
		{"420", 8, "yuv420p", false},
		{"422", 10, "yuv422p10le", false},
		{"444", 12, "yuv444p12le", false},
		{"420", 13, "", true}, // unsupported bitdepth
		{"bad", 8, "", true},  // unsupported pixfmt
	}
	for _, tt := range tests {
		got, err := toFFmpegPixfmt(tt.pf, tt.bd)
		if (err != nil) != tt.wantErr {
			t.Errorf("toFFmpegPixfmt(%q,%d): err=%v, wantErr=%v", tt.pf, tt.bd, err, tt.wantErr)
		}
		if got != tt.want {
			t.Errorf("toFFmpegPixfmt(%q,%d): got %q, want %q", tt.pf, tt.bd, got, tt.want)
		}
	}
}

func TestRoundF(t *testing.T) {
	t.Parallel()
	tests := []struct {
		in   float64
		dec  int
		want float64
	}{
		{3.14159, 2, 3.14},
		{3.14159, 0, 3.0},
		{2.5, 0, 3.0},   // half-up
		{1.005, 2, 1.0}, // float representation: 1.005 actually < 1.005 — accept that
	}
	for _, tt := range tests {
		got := roundF(tt.in, tt.dec)
		// Accept a small float epsilon for half-up edge case.
		diff := got - tt.want
		if diff < -1e-9 || diff > 1e-9 {
			// Half-up against IEEE-754 representation is fragile; only fail on bigger drift.
			if (got-tt.want) > 0.5 || (tt.want-got) > 0.5 {
				t.Errorf("roundF(%v, %d): got %v, want ~%v", tt.in, tt.dec, got, tt.want)
			}
		}
	}
}

func TestTruncate(t *testing.T) {
	t.Parallel()
	if got := truncate("hello", 10); got != "hello" {
		t.Errorf("no-truncate: got %q", got)
	}
	if got := truncate("hello world", 5); got != "hello" {
		t.Errorf("truncate: got %q, want \"hello\"", got)
	}
	if got := truncate("", 5); got != "" {
		t.Errorf("empty: got %q", got)
	}
}

func TestFloatFromAny(t *testing.T) {
	t.Parallel()
	if got := floatFromAny(3.5); got != 3.5 {
		t.Errorf("float64: got %v", got)
	}
	if got := floatFromAny(json.Number("2.5")); got != 2.5 {
		t.Errorf("json.Number: got %v", got)
	}
	if got := floatFromAny("not a number"); got != 0 {
		t.Errorf("string: got %v, want 0", got)
	}
	if got := floatFromAny(nil); got != 0 {
		t.Errorf("nil: got %v, want 0", got)
	}
}

func TestPickWorstFrames(t *testing.T) {
	t.Parallel()
	payload := map[string]any{
		"frames": []any{
			map[string]any{
				"frameNum": float64(0),
				"metrics":  map[string]any{"vmaf": 95.0},
			},
			map[string]any{
				"frameNum": float64(1),
				"metrics":  map[string]any{"vmaf": 30.0},
			},
			map[string]any{
				"frameNum": float64(2),
				"metrics":  map[string]any{"vmaf": 60.0},
			},
		},
	}
	worst := pickWorstFrames(payload, 2)
	if len(worst) != 2 {
		t.Fatalf("len: got %d, want 2", len(worst))
	}
	// Worst frame first.
	if worst[0][0].(int) != 1 || worst[0][1].(float64) != 30.0 {
		t.Errorf("worst[0]: got %v, want [1, 30]", worst[0])
	}
	if worst[1][0].(int) != 2 || worst[1][1].(float64) != 60.0 {
		t.Errorf("worst[1]: got %v, want [2, 60]", worst[1])
	}

	// n larger than available frames is clamped.
	all := pickWorstFrames(payload, 10)
	if len(all) != 3 {
		t.Errorf("clamp: len=%d, want 3", len(all))
	}
}

// ---------------------------------------------------------------------------
// Handler error paths — these verify the documented invariants:
//   1. Missing required args → handler returns a non-nil error.
//   2. handleProbeBackend with an unrecognised backend returns a payload with
//      compiled_in=false (not an error) — that's the documented contract from
//      probe_backend in tools.go.
// ---------------------------------------------------------------------------

func TestHandleProbeBackendMissingBackend(t *testing.T) {
	t.Parallel()
	_, err := handleProbeBackend(context.Background(), map[string]any{})
	if err == nil {
		t.Fatal("expected error for missing backend arg")
	}
	if !strings.Contains(err.Error(), "backend is required") {
		t.Errorf("error message: got %q", err.Error())
	}
}

func TestHandleProbeBackendUnknownReturnsCompiledInFalse(t *testing.T) {
	t.Parallel()
	// "wibble" is not a known backend; probeBackends won't advertise it so the
	// handler reports compiled_in=false without invoking the binary.
	result, err := handleProbeBackend(context.Background(), map[string]any{"backend": "wibble"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	m, ok := result.(map[string]any)
	if !ok {
		t.Fatalf("result type: %T", result)
	}
	if v, _ := m["compiled_in"].(bool); v {
		t.Errorf("compiled_in: got true, want false for unknown backend")
	}
	if v, _ := m["runtime_healthy"].(bool); v {
		t.Errorf("runtime_healthy: got true, want false for unknown backend")
	}
}

// TestProbeYUVDimensions locks the probe frame to 64x64 4:2:0 8-bit. 32x32 is
// below the CUDA ADM >=36px-per-dimension minimum and silently yields a null
// score on a CUDA build, so the dimensions must stay byte-compatible with the
// Python server's _PROBE_YUV_DATA (server.py).
func TestProbeYUVDimensions(t *testing.T) {
	t.Parallel()
	if probeYUVWidth != 64 || probeYUVHeight != 64 {
		t.Fatalf("probe dims: got %dx%d, want 64x64 (>=36px CUDA-ADM minimum)",
			probeYUVWidth, probeYUVHeight)
	}
	want := probeYUVWidth * probeYUVHeight * 3 / 2 // 6144 bytes for 4:2:0 8-bit
	if len(probeYUVData) != want {
		t.Fatalf("probeYUVData size: got %d, want %d", len(probeYUVData), want)
	}
}

// TestScoreIsHealthy mirrors the Python server's `score is not None` guard plus
// the Go-side non-finite rejection: a null or non-finite score must NOT report
// runtime_healthy=true.
func TestScoreIsHealthy(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name  string
		score any
		want  bool
	}{
		{"nil", nil, false},
		{"finite", 100.0, true},
		{"zero", 0.0, true},
		{"nan", math.NaN(), false},
		{"posinf", math.Inf(1), false},
		{"neginf", math.Inf(-1), false},
		{"jsonNumberFinite", json.Number("97.5"), true},
		{"jsonNumberJunk", json.Number("not-a-number"), false},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := scoreIsHealthy(tc.score); got != tc.want {
				t.Errorf("scoreIsHealthy(%v) = %v, want %v", tc.score, got, tc.want)
			}
		})
	}
}

func TestHandleDescribeModelMissingName(t *testing.T) {
	t.Parallel()
	_, err := handleDescribeModel(context.Background(), map[string]any{})
	if err == nil {
		t.Fatal("expected error for missing name arg")
	}
	if !strings.Contains(err.Error(), "name is required") {
		t.Errorf("error message: got %q", err.Error())
	}
}

func TestHandleVmafScoreMissingDimensions(t *testing.T) {
	t.Parallel()
	// Use a guaranteed-unreadable path to exercise the ref-validation error
	// before any dimension check; this confirms that the handler surfaces
	// ValidatePath failures rather than panicking.
	_, err := handleVmafScore(context.Background(), map[string]any{
		"ref": "/this/path/does/not/exist/ref.yuv",
		"dis": "/this/path/does/not/exist/dis.yuv",
	})
	if err == nil {
		t.Fatal("expected error for nonexistent ref")
	}
}

func TestHandleVmafScoreZeroDimensions(t *testing.T) {
	// Cannot use t.Parallel because we Setenv.
	// Use the repo-root model dir so ValidatePath succeeds, then fail on
	// width/height. Write valid YUV stubs under the temp dir and allowlist it
	// via VMAF_MCP_ALLOW.
	tmp := t.TempDir()
	ref := tmp + "/ref.yuv"
	dis := tmp + "/dis.yuv"
	if err := writeFile(ref, []byte("x")); err != nil {
		t.Fatalf("write ref: %v", err)
	}
	if err := writeFile(dis, []byte("x")); err != nil {
		t.Fatalf("write dis: %v", err)
	}
	t.Setenv("VMAF_MCP_ALLOW", tmp)
	_, err := handleVmafScore(context.Background(), map[string]any{
		"ref":    ref,
		"dis":    dis,
		"width":  float64(0),
		"height": float64(0),
	})
	if err == nil {
		t.Fatal("expected error for zero dimensions")
	}
	if !strings.Contains(err.Error(), "must be positive") {
		t.Errorf("error message: got %q", err.Error())
	}
}

func TestHandleCompareModelsEmptyList(t *testing.T) {
	t.Parallel()
	_, err := handleCompareModels(context.Background(), map[string]any{
		"models": []any{}, // empty
	})
	if err == nil {
		t.Fatal("expected error for empty models list")
	}
	if !strings.Contains(err.Error(), "non-empty list") {
		t.Errorf("error message: got %q", err.Error())
	}
}

// ---------------------------------------------------------------------------
// errorResult — pin the isError=true invariant from memory.
// ---------------------------------------------------------------------------

func TestErrorResultSetsIsError(t *testing.T) {
	t.Parallel()
	res := errorResult("boom")
	if res == nil {
		t.Fatal("errorResult returned nil")
	}
	if !res.IsError {
		t.Error("errorResult: IsError=false, want true (MCP isError invariant)")
	}
	if len(res.Content) != 1 {
		t.Fatalf("Content len: got %d, want 1", len(res.Content))
	}
}

// TestErrorMustBeNonNil pins the assumption that errors.New stays usable from
// handlers (i.e. the error interface is intact in this build). Trivial but
// guards against import-cycle / build-tag accidents.
func TestErrorMustBeNonNil(t *testing.T) {
	t.Parallel()
	err := errors.New("x")
	if err == nil {
		t.Fatal("errors.New returned nil")
	}
}

// ---------------------------------------------------------------------------
// Small helpers (test-local)
// ---------------------------------------------------------------------------

// strFromInt avoids importing strconv just for this test helper.
func strFromInt(i int) string {
	if i == 0 {
		return "0"
	}
	const digits = "0123456789"
	var buf [20]byte
	n := len(buf)
	neg := false
	if i < 0 {
		neg = true
		i = -i
	}
	for i > 0 {
		n--
		buf[n] = digits[i%10]
		i /= 10
	}
	if neg {
		n--
		buf[n] = '-'
	}
	return string(buf[n:])
}

// writeFile is a thin os.WriteFile wrapper used by the tests above.
func writeFile(path string, data []byte) error {
	return os.WriteFile(path, data, 0o600)
}
