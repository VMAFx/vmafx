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

// TestSidecarArgvParity pins the Go and Python sidecar-bridge argv builders to
// byte-for-byte equality, the same contract TestGoAndPythonArgvParity enforces
// for the score tools (cmd/vmafx-mcp/AGENTS.md invariant #15).
//
// The builders validate their path arguments, so the fixtures below are real
// allowlisted files (model/*.json) rather than synthetic paths.
func TestSidecarArgvParity(t *testing.T) {
	repoRoot := findRepoRoot(t)
	pyModuleDir := filepath.Join(repoRoot, "mcp-server", "vmaf-mcp", "src")

	pythonBin := os.Getenv("PYTHON_BIN")
	if pythonBin == "" {
		pythonBin = "python3"
	}
	if _, err := exec.LookPath(pythonBin); err != nil {
		t.Skipf("python3 not found; skipping sidecar argv parity test")
	}
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

	// A real, allowlisted file usable as any path argument the builders validate.
	fixture := filepath.Join(repoRoot, "model", "vmaf_v0.6.1.json")
	if _, err := os.Stat(fixture); err != nil {
		t.Skipf("fixture %s missing: %v", fixture, err)
	}

	pyRunner := fmt.Sprintf(`
import json, sys
sys.path.insert(0, %q)
from vmaf_mcp import server as srv

payload = json.loads(sys.stdin.read())
tool = payload["tool"]
args = payload["args"]
if tool == "vmaf_per_shot":
    argv, _ = srv._build_per_shot_argv("BIN", args)
elif tool == "vmaf_roi":
    argv, _ = srv._build_roi_argv("BIN", payload["out_path"], args)
elif tool == "vmaf_bench":
    argv, _ = srv._build_bench_argv("BIN", args)
elif tool == "vmaf_vpl":
    argv, _ = srv._build_vpl_argv("BIN", args)
else:
    raise SystemExit("unknown tool " + tool)
print(json.dumps(argv))
`, pyModuleDir)

	const outPath = "/tmp/vmaf-mcp-roi-parity.bin"

	cases := []struct {
		name string
		tool string
		args map[string]any
	}{
		{
			name: "per_shot_defaults",
			tool: "vmaf_per_shot",
			args: map[string]any{
				"reference": fixture,
				"width":     float64(576),
				"height":    float64(324),
			},
		},
		{
			name: "per_shot_all_options_csv",
			tool: "vmaf_per_shot",
			args: map[string]any{
				"reference":      fixture,
				"width":          float64(1920),
				"height":         float64(1080),
				"pixel_format":   "422",
				"bitdepth":       float64(10),
				"target_vmaf":    float64(93.5),
				"crf_min":        float64(20),
				"crf_max":        float64(40),
				"diff_threshold": float64(7.25),
				"format":         "csv",
			},
		},
		{
			name: "per_shot_integral_float_formatting",
			tool: "vmaf_per_shot",
			args: map[string]any{
				// Pins the Go strconv.FormatFloat('f', -1) vs Python _fmt_float
				// agreement: both must emit "88", never "88.0".
				"reference":      fixture,
				"width":          float64(640),
				"height":         float64(480),
				"target_vmaf":    float64(88),
				"diff_threshold": float64(12),
			},
		},
		{
			name: "roi_defaults_x265",
			tool: "vmaf_roi",
			args: map[string]any{
				"reference": fixture,
				"width":     float64(1920),
				"height":    float64(1080),
				"frame":     float64(0),
			},
		},
		{
			name: "roi_svtav1_with_saliency",
			tool: "vmaf_roi",
			args: map[string]any{
				"reference":      fixture,
				"width":          float64(3840),
				"height":         float64(2160),
				"frame":          float64(17),
				"pixel_format":   "444",
				"bitdepth":       float64(12),
				"ctu_size":       float64(32),
				"encoder":        "svt-av1",
				"strength":       float64(9.5),
				"saliency_model": fixture,
			},
		},
		{
			name: "bench_empty",
			tool: "vmaf_bench",
			args: map[string]any{},
		},
		{
			name: "bench_all_flags",
			tool: "vmaf_bench",
			args: map[string]any{
				"frames":      float64(24),
				"resolution":  "1920x1080",
				"bpc":         float64(10),
				"validate":    true,
				"gpu_only":    true,
				"device_list": true,
			},
		},
		{
			name: "vpl_defaults",
			tool: "vmaf_vpl",
			args: map[string]any{
				"ref": fixture,
				"dis": fixture,
			},
		},
		{
			name: "vpl_all_flags",
			tool: "vmaf_vpl",
			args: map[string]any{
				"ref":         fixture,
				"dis":         fixture,
				"model":       "vmaf_4k_v0.6.1",
				"frames":      float64(120),
				"device":      float64(1),
				"render_node": "/dev/dri/renderD129",
				"fallback":    true,
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var goArgv []string
			var err error
			switch tc.tool {
			case "vmaf_per_shot":
				goArgv, _, err = buildPerShotArgv("BIN", tc.args)
			case "vmaf_roi":
				goArgv, _, err = buildRoiArgv("BIN", outPath, tc.args)
			case "vmaf_bench":
				goArgv, _, err = buildBenchArgv("BIN", tc.args)
			case "vmaf_vpl":
				goArgv, _, err = buildVplArgv("BIN", tc.args)
			}
			if err != nil {
				t.Fatalf("Go builder failed: %v", err)
			}

			in, marshalErr := json.Marshal(map[string]any{
				"tool": tc.tool, "args": tc.args, "out_path": outPath,
			})
			if marshalErr != nil {
				t.Fatalf("json.Marshal: %v", marshalErr)
			}
			cmd := exec.CommandContext(context.Background(), pythonBin, "-c", pyRunner)
			cmd.Stdin = bytes.NewReader(in)
			var stderr bytes.Buffer
			cmd.Stderr = &stderr
			out, runErr := cmd.Output()
			if runErr != nil {
				t.Fatalf("Python runner failed: %v: %s", runErr, stderr.String())
			}
			var pyArgv []string
			if unmarshalErr := json.Unmarshal(bytes.TrimSpace(out), &pyArgv); unmarshalErr != nil {
				t.Fatalf("json.Unmarshal pyArgv: %v\nraw: %s", unmarshalErr, string(out))
			}
			if !reflect.DeepEqual(goArgv, pyArgv) {
				t.Errorf("argv parity mismatch for %s:\nGo:     %v\nPython: %v",
					tc.name, goArgv, pyArgv)
			}
		})
	}
}
