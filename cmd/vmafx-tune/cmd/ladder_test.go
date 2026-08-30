// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd_test

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// TestLadder_helpFlag verifies that "vmafx-tune-go ladder --help" exits 0 and
// mentions key flags.
func TestLadder_helpFlag(t *testing.T) {
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

	out, err := exec.Command(binPath, "ladder", "--help").CombinedOutput()
	if err != nil {
		t.Fatalf("ladder --help exited non-zero: %v\n%s", err, out)
	}

	helpText := string(out)
	for _, want := range []string{
		"--reference", "--codec", "--targets", "--resolutions",
		"--max-rungs", "--output", "--format",
	} {
		if !strings.Contains(helpText, want) {
			t.Errorf("ladder --help missing flag %q", want)
		}
	}
}

// TestLadder_missingReference verifies that the ladder subcommand exits
// non-zero when --reference is missing or the file does not exist.
func TestLadder_missingReference(t *testing.T) {
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

	cmd := exec.Command(binPath, "ladder",
		"--reference", "/nonexistent/path/src.mp4",
		"--codec", "libx264",
		"--targets", "85",
		"--resolutions", "640x480",
	)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatal("expected non-zero exit for missing reference, got success")
	}
	t.Logf("stderr: %s", out)
}

// TestLadder_invalidResolution verifies that malformed --resolutions flag
// values are rejected.
func TestLadder_invalidResolution(t *testing.T) {
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

	// Create a dummy reference file so we get past the file-existence check.
	srcPath := filepath.Join(dir, "fake.mp4")
	if err := os.WriteFile(srcPath, []byte("fake"), 0o644); err != nil {
		t.Fatalf("create fake source: %v", err)
	}

	cmd := exec.Command(binPath, "ladder",
		"--reference", srcPath,
		"--codec", "libx264",
		"--targets", "85",
		"--resolutions", "not_a_resolution", // invalid
	)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatalf("expected non-zero exit for invalid resolution, got success\noutput: %s", out)
	}
}

// TestLadder_outputSchemaJSON runs an integration ladder sweep when both
// ffmpeg and vmaf are available on PATH and validates the JSON output schema.
func TestLadder_outputSchemaJSON(t *testing.T) {
	t.Parallel()

	for _, bin := range []string{"ffmpeg", "vmaf"} {
		if _, err := exec.LookPath(bin); err != nil {
			t.Skipf("%s not found on PATH — skipping integration ladder test", bin)
		}
	}

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

	outJSON := filepath.Join(dir, "ladder.json")

	// Run ladder with a single resolution and single target to keep it fast.
	runOut, runErr := exec.Command(
		binPath, "ladder",
		"--reference", srcPath,
		"--codec", "libx264",
		"--targets", "60",
		"--resolutions", "320x240",
		"--output", outJSON,
		"--format", "json",
		"--work-dir", dir,
		"--max-rungs", "3",
	).CombinedOutput()
	if runErr != nil {
		t.Fatalf("vmafx-tune-go ladder: %v\n%s", runErr, runOut)
	}

	data, readErr := os.ReadFile(outJSON)
	if readErr != nil {
		t.Fatalf("read output JSON: %v", readErr)
	}

	var payload map[string]any
	if parseErr := json.Unmarshal(data, &payload); parseErr != nil {
		t.Fatalf("parse output JSON: %v\n%s", parseErr, string(data))
	}

	// Validate required top-level keys.
	for _, key := range []string{
		"schema_version", "src", "encoder", "target_vmafs",
		"tool_version", "wall_time_ms", "cloud", "hull", "renditions",
	} {
		if _, ok := payload[key]; !ok {
			t.Errorf("output JSON missing required key %q", key)
		}
	}

	// schema_version must be 1.
	if v, ok := payload["schema_version"].(float64); !ok || int(v) != 1 {
		t.Errorf("schema_version = %v, want 1", payload["schema_version"])
	}

	t.Logf("ladder output: %s", string(data))
}
