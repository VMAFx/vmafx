// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// TestCompare_outputShapeJSON builds the vmafx-tune-go binary against a
// synthetic lavfi source and validates the JSON output has the expected schema
// shape. The test is skipped when ffmpeg or vmaf is not on PATH.
func TestCompare_outputShapeJSON(t *testing.T) {
	t.Parallel()

	for _, bin := range []string{"ffmpeg", "vmaf"} {
		if _, err := exec.LookPath(bin); err != nil {
			t.Skipf("%s not found on PATH — skipping integration compare test", bin)
		}
	}

	// Build the binary into a temp dir.
	dir := t.TempDir()
	binPath := filepath.Join(dir, "vmafx-tune-go")
	buildOut, buildErr := exec.Command(
		"go", "build",
		"-o", binPath,
		"github.com/VMAFx/vmafx/cmd/vmafx-tune",
	).CombinedOutput()
	if buildErr != nil {
		t.Fatalf("build vmafx-tune-go: %v\n%s", buildErr, buildOut)
	}

	// Generate a short synthetic y4m source.
	srcPath := filepath.Join(dir, "src.y4m")
	genOut, genErr := exec.Command(
		"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
		"-f", "lavfi",
		"-i", "testsrc=size=320x240:rate=24:duration=1",
		"-pix_fmt", "yuv420p",
		srcPath,
	).CombinedOutput()
	if genErr != nil {
		t.Skipf("failed to generate synthetic source (%v): %s", genErr, genOut)
	}

	outJSON := filepath.Join(dir, "result.json")

	// Run compare with libx264 only (libx265 would double the encode time).
	runOut, runErr := exec.Command(
		binPath, "compare",
		"--reference", srcPath,
		"--codecs", "libx264",
		"--targets", "60",
		"--output", outJSON,
		"--format", "json",
		"--work-dir", dir,
	).CombinedOutput()
	if runErr != nil {
		t.Fatalf("vmafx-tune-go compare: %v\n%s", runErr, runOut)
	}

	// Read and parse the JSON output.
	data, readErr := os.ReadFile(outJSON)
	if readErr != nil {
		t.Fatalf("read output JSON: %v", readErr)
	}

	var payload map[string]any
	if parseErr := json.Unmarshal(data, &payload); parseErr != nil {
		t.Fatalf("parse output JSON: %v\n%s", parseErr, string(data))
	}

	// Validate required top-level keys.
	for _, key := range []string{"src", "target_vmaf", "tool_version", "wall_time_ms", "rows"} {
		if _, ok := payload[key]; !ok {
			t.Errorf("output JSON missing required key %q", key)
		}
	}

	rows, ok := payload["rows"].([]any)
	if !ok {
		t.Fatalf("output JSON 'rows' is not an array")
	}
	if len(rows) == 0 {
		t.Fatal("output JSON 'rows' is empty")
	}

	// Validate the first row has the expected fields.
	row, ok := rows[0].(map[string]any)
	if !ok {
		t.Fatalf("rows[0] is not an object")
	}
	for _, key := range []string{"codec", "best_crf", "ok", "target_vmaf"} {
		if _, ok := row[key]; !ok {
			t.Errorf("rows[0] missing required key %q", key)
		}
	}

	t.Logf("compare output: %s", string(data))
}

// TestCompare_missingReference verifies that the compare subcommand returns
// an error when --reference is missing or the file does not exist.
func TestCompare_missingReference(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	binPath := filepath.Join(dir, "vmafx-tune-go")
	buildOut, buildErr := exec.Command(
		"go", "build",
		"-o", binPath,
		"github.com/VMAFx/vmafx/cmd/vmafx-tune",
	).CombinedOutput()
	if buildErr != nil {
		t.Fatalf("build vmafx-tune-go: %v\n%s", buildErr, buildOut)
	}

	var out bytes.Buffer
	cmd := exec.Command(binPath, "compare",
		"--reference", "/nonexistent/path/src.mp4",
		"--codecs", "libx264",
		"--targets", "85",
	)
	cmd.Stdout = &out
	cmd.Stderr = &out
	err := cmd.Run()
	if err == nil {
		t.Fatal("expected non-zero exit for missing reference, got success")
	}
	if !strings.Contains(out.String(), "nonexistent") &&
		!strings.Contains(out.String(), "reference") {
		t.Logf("stderr: %s", out.String())
	}
}

// TestStubSubcommands verifies that unported subcommands exit non-zero and
// emit a redirect message.
func TestStubSubcommands(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	binPath := filepath.Join(dir, "vmafx-tune-go")
	buildOut, buildErr := exec.Command(
		"go", "build",
		"-o", binPath,
		"github.com/VMAFx/vmafx/cmd/vmafx-tune",
	).CombinedOutput()
	if buildErr != nil {
		t.Fatalf("build vmafx-tune-go: %v\n%s", buildErr, buildOut)
	}

	// Ported so far: ladder, report, tune-per-shot (group 1), fast (group 2)
	// and the ML-driven four (group 6). These exit non-zero only when required
	// flags are missing, not as stubs, so they are excluded here.
	for _, sub := range []string{"corpus", "benchmark", "auto", "sidecar"} {
		sub := sub
		t.Run(sub, func(t *testing.T) {
			t.Parallel()
			var out bytes.Buffer
			cmd := exec.Command(binPath, sub)
			cmd.Stdout = &out
			cmd.Stderr = &out
			err := cmd.Run()
			if err == nil {
				t.Errorf("stub subcommand %q should exit non-zero, got success", sub)
			}
			if !strings.Contains(out.String(), "vmaf-tune") {
				t.Errorf("stub subcommand %q should mention 'vmaf-tune' in output, got: %q",
					sub, out.String())
			}
			t.Logf("%s output: %s", sub, out.String())
		})
	}
}

// TestVersion verifies that --version prints a version string and exits 0.
func TestVersion(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	binPath := filepath.Join(dir, "vmafx-tune-go")
	buildOut, buildErr := exec.Command(
		"go", "build",
		"-o", binPath,
		"github.com/VMAFx/vmafx/cmd/vmafx-tune",
	).CombinedOutput()
	if buildErr != nil {
		t.Fatalf("build vmafx-tune-go: %v\n%s", buildErr, buildOut)
	}

	out, err := exec.Command(binPath, "--version").CombinedOutput()
	if err != nil {
		t.Fatalf("--version exited non-zero: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "vmafx-tune-go") {
		t.Errorf("--version output should contain binary name, got: %q",
			fmt.Sprintf("%.80s", string(out)))
	}
}
