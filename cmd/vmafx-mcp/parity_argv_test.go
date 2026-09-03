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
	"path/filepath"
	"reflect"
	"testing"
)

// TestGoAndPythonArgvParity verifies that the Go server's buildVmafArgv and the
// Python server's _build_vmaf_argv produce identical, byte-compatible argv
// slices for all combinations of scoring parameters and tiny-AI flags.
func TestGoAndPythonArgvParity(t *testing.T) {
	repoRoot := findRepoRoot(t)
	pyModuleDir := filepath.Join(repoRoot, "mcp-server", "vmaf-mcp", "src")

	// Locate python3 interpreter with mcp installed.
	pythonBin := os.Getenv("PYTHON_BIN")
	if pythonBin == "" {
		candidates := []string{
			repoRoot + "/.claude/worktrees/tmp-mcpvenv/bin/python3",
			"/home/kilian/dev/vmaf/.claude/worktrees/tmp-mcpvenv/bin/python3",
			"python3",
		}
		for _, c := range candidates {
			if _, err := exec.LookPath(c); err == nil {
				pythonBin = c
				break
			}
		}
	}
	if pythonBin == "" {
		t.Skip("python3 not found; skipping Go/Python argv parity test")
	}

	// Verify Python can import vmaf_mcp.server.
	checkCmd := exec.Command(pythonBin, "-c", fmt.Sprintf(`
import sys
sys.path.insert(0, %q)
try:
    import vmaf_mcp.server
except Exception:
    sys.exit(1)
`, pyModuleDir))
	if err := checkCmd.Run(); err != nil {
		t.Skipf("python %s cannot import vmaf_mcp.server: %v", pythonBin, err)
	}

	testCases := []struct {
		name string
		args map[string]any
	}{
		{
			name: "plain_minimal_fr",
			args: map[string]any{
				"ref":       "/data/ref.yuv",
				"dis":       "/data/dis.yuv",
				"width":     float64(1920),
				"height":    float64(1080),
				"pixfmt":    "420",
				"bitdepth":  float64(8),
				"model":     "version=vmaf_v0.6.1",
				"backend":   "auto",
				"precision": "legacy",
			},
		},
		{
			name: "cpu_backend_disables_siblings",
			args: map[string]any{
				"ref":       "/data/ref.yuv",
				"dis":       "/data/dis.yuv",
				"width":     float64(1920),
				"height":    float64(1080),
				"pixfmt":    "420",
				"bitdepth":  float64(8),
				"backend":   "cpu",
				"precision": "legacy",
			},
		},
		{
			name: "cuda_backend_disables_siblings",
			args: map[string]any{
				"ref":       "/data/ref.yuv",
				"dis":       "/data/dis.yuv",
				"width":     float64(1920),
				"height":    float64(1080),
				"pixfmt":    "422",
				"bitdepth":  float64(10),
				"backend":   "cuda",
				"precision": "max",
			},
		},
		{
			name: "tiny_ai_full_suite",
			args: map[string]any{
				"ref":               "/data/ref.yuv",
				"dis":               "/data/dis.yuv",
				"width":             float64(3840),
				"height":            float64(2160),
				"pixfmt":            "444",
				"bitdepth":          float64(12),
				"tiny_model":        "/models/nr_metric.onnx",
				"tiny_device":       "cuda",
				"tiny_threads":      float64(4),
				"tiny_fp16":         true,
				"tiny_model_verify": true,
				"tiny_codec":        "libx264",
				"tiny_preset":       "veryslow",
				"tiny_crf":          float64(18),
				"tiny_resize":       "bicubic",
				"backend":           "cpu",
			},
		},
		{
			name: "dnn_ep_alias_maps_to_tiny_device",
			args: map[string]any{
				"ref":         "/data/ref.yuv",
				"dis":         "/data/dis.yuv",
				"width":       float64(1280),
				"height":      float64(720),
				"pixfmt":      "420",
				"bitdepth":    float64(8),
				"tiny_model":  "/models/nr_metric.onnx",
				"dnn_ep":      "openvino-npu",
				"tiny_resize": "nearest",
			},
		},
		{
			name: "no_reference_mode_omits_ref",
			args: map[string]any{
				"dis":          "/data/dis.yuv",
				"width":        float64(1920),
				"height":       float64(1080),
				"pixfmt":       "420",
				"bitdepth":     float64(8),
				"tiny_model":   "/models/nr.onnx",
				"no_reference": true,
			},
		},
		{
			name: "no_reference_with_ref_passed",
			args: map[string]any{
				"ref":          "/data/ref.yuv",
				"dis":          "/data/dis.yuv",
				"width":        float64(1920),
				"height":       float64(1080),
				"pixfmt":       "420",
				"bitdepth":     float64(8),
				"tiny_model":   "/models/nr.onnx",
				"no_reference": true,
			},
		},
		{
			name: "features_and_ctc_and_threads",
			args: map[string]any{
				"ref":             "/data/ref.yuv",
				"dis":             "/data/dis.yuv",
				"width":           float64(1920),
				"height":          float64(1080),
				"pixfmt":          "420",
				"bitdepth":        float64(8),
				"feature":         []any{"psnr", "cambi=full_ref=true", "ciede"},
				"aom_ctc":         "v5.0",
				"threads":         float64(16),
				"frame_cnt":       float64(500),
				"frame_skip_ref":  float64(5),
				"frame_skip_dist": float64(0), // explicit 0
				"no_prediction":   true,
			},
		},
		{
			name: "subsample_ordering",
			args: map[string]any{
				"ref":       "/data/ref.yuv",
				"dis":       "/data/dis.yuv",
				"width":     float64(1920),
				"height":    float64(1080),
				"pixfmt":    "420",
				"bitdepth":  float64(8),
				"subsample": float64(4),
				"feature":   []any{"psnr"},
			},
		},
	}

	pyRunnerScript := fmt.Sprintf(`
import json, sys
sys.path.insert(0, %q)
from vmaf_mcp.server import _extras_from_args, ScoreRequest, _build_vmaf_argv
from pathlib import Path

args = json.loads(sys.stdin.read())
extras = _extras_from_args(args)

ref_arg = args.get("ref")
ref_path = Path(ref_arg) if ref_arg else None

req = ScoreRequest(
    ref=ref_path,
    dis=Path(args["dis"]),
    width=int(args["width"]),
    height=int(args["height"]),
    pixfmt=str(args.get("pixfmt", "420")),
    bitdepth=int(args.get("bitdepth", 8)),
    model=str(args.get("model", "version=vmaf_v0.6.1")),
    backend=str(args.get("backend", "auto")),
    precision=str(args.get("precision", "legacy")),
    subsample=int(args.get("subsample", 1)),
    extras=extras,
)
argv = _build_vmaf_argv(req, vmaf="vmaf", output=args["out_path"])
print(json.dumps(argv))
`, pyModuleDir)

	for _, tc := range testCases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			outPath := "/tmp/vmaf-out.json"
			tc.args["out_path"] = outPath

			// 1. Build Go argv
			ex, err := parseScoreExtras(tc.args)
			if err != nil {
				t.Fatalf("Go parseScoreExtras failed: %v", err)
			}

			ref := strArg(tc.args, "ref", "")
			dis := strArg(tc.args, "dis", "")
			width := intArg(tc.args, "width", 0)
			height := intArg(tc.args, "height", 0)
			pixfmt := strArg(tc.args, "pixfmt", "420")
			bitdepth := intArg(tc.args, "bitdepth", 8)
			model := strArg(tc.args, "model", "version=vmaf_v0.6.1")
			backend := strArg(tc.args, "backend", "auto")
			precision := strArg(tc.args, "precision", "legacy")

			goArgv := buildVmafArgv("vmaf", ref, dis, width, height, pixfmt, bitdepth, model, backend, precision, outPath, ex)

			// 2. Build Python argv
			inBytes, err := json.Marshal(tc.args)
			if err != nil {
				t.Fatalf("json.Marshal: %v", err)
			}

			cmd := exec.CommandContext(context.Background(), pythonBin, "-c", pyRunnerScript)
			cmd.Stdin = bytes.NewReader(inBytes)
			var stderr bytes.Buffer
			cmd.Stderr = &stderr
			outBytes, err := cmd.Output()
			if err != nil {
				t.Fatalf("Python runner failed: %v: %s", err, stderr.String())
			}

			var pyArgv []string
			if err := json.Unmarshal(bytes.TrimSpace(outBytes), &pyArgv); err != nil {
				t.Fatalf("json.Unmarshal pyArgv: %v\nraw: %s", err, string(outBytes))
			}

			// 3. Assert byte-for-byte parity
			if !reflect.DeepEqual(goArgv, pyArgv) {
				t.Errorf("Parity mismatch for %s:\nGo:     %v\nPython: %v", tc.name, goArgv, pyArgv)
			}
		})
	}
}
