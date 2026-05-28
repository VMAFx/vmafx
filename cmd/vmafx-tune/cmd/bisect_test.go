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

// TestBisect_helpFlag verifies that "vmafx-tune-go bisect --help" exits 0
// and mentions key flags.
func TestBisect_helpFlag(t *testing.T) {
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

	out, err := exec.Command(binPath, "bisect", "--help").CombinedOutput()
	if err != nil {
		t.Fatalf("bisect --help exited non-zero: %v\n%s", err, out)
	}

	helpText := string(out)
	for _, want := range []string{
		"--reference", "--encoder", "--target-vmaf",
		"--bitrate-min", "--bitrate-max", "--tolerance",
		"--output", "--format",
	} {
		if !strings.Contains(helpText, want) {
			t.Errorf("bisect --help missing flag %q", want)
		}
	}
}

// TestBisect_missingReference verifies that the bisect subcommand exits
// non-zero when --reference is missing.
func TestBisect_missingReference(t *testing.T) {
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

	cmd := exec.Command(binPath, "bisect",
		"--reference", "/nonexistent/path/src.mp4",
		"--encoder", "libx264",
		"--target-vmaf", "85",
	)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatal("expected non-zero exit for missing reference, got success")
	}
	t.Logf("stderr: %s", out)
}

// TestBisect_invalidBitrateRange verifies that bitrate-max ≤ bitrate-min is rejected.
func TestBisect_invalidBitrateRange(t *testing.T) {
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

	// Create a dummy reference file.
	srcPath := filepath.Join(dir, "fake.mp4")
	if err := os.WriteFile(srcPath, []byte("fake"), 0o644); err != nil {
		t.Fatalf("create fake source: %v", err)
	}

	cmd := exec.Command(binPath, "bisect",
		"--reference", srcPath,
		"--encoder", "libx264",
		"--target-vmaf", "85",
		"--bitrate-min", "5000",
		"--bitrate-max", "1000", // max < min — invalid
	)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatalf("expected non-zero exit for inverted bitrate range, got success\noutput: %s", out)
	}
}

// TestBisect_outputSchemaJSON runs an integration bisect when both ffmpeg and
// vmaf are available on PATH and validates the JSON output schema.
//
// Note: this test requires a vmaf binary that accepts container formats
// (MKV/MP4) as distorted input.  The fork's vmaf CLI only accepts .y4m/.yuv
// so this test is skipped on machines where the vmaf binary rejects MKV
// distorted files (detected via a preflight probe).
func TestBisect_outputSchemaJSON(t *testing.T) {
	t.Parallel()

	for _, bin := range []string{"ffmpeg", "vmaf"} {
		if _, err := exec.LookPath(bin); err != nil {
			t.Skipf("%s not found on PATH — skipping integration bisect test", bin)
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

	// Preflight: encode a tiny MKV and check whether the vmaf binary accepts
	// it as a distorted input.  The fork's vmaf CLI only reads .y4m/.yuv; if
	// the probe fails, skip rather than fail.
	probeMKV := filepath.Join(dir, "probe.mkv")
	probeEnc, probeEncErr := exec.Command(
		"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
		"-i", srcPath, "-an", "-c:v", "libx264", "-crf", "35", probeMKV,
	).CombinedOutput()
	if probeEncErr != nil {
		t.Skipf("ffmpeg encode probe failed (%v): %s", probeEncErr, probeEnc)
	}
	probeXML := filepath.Join(dir, "probe_score.xml")
	probeScore, probeScoreErr := exec.Command(
		"vmaf",
		"--reference", srcPath,
		"--distorted", probeMKV,
		"--output", probeXML,
		"--xml",
	).CombinedOutput()
	if probeScoreErr != nil {
		t.Skipf("vmaf binary does not accept MKV distorted input — skipping integration test: %s", probeScore)
	}

	outJSON := filepath.Join(dir, "bisect.json")

	// Use a low target VMAF (50) so the bisect converges quickly at low bitrate.
	runOut, runErr := exec.Command(
		binPath, "bisect",
		"--reference", srcPath,
		"--encoder", "libx264",
		"--target-vmaf", "50",
		"--bitrate-min", "200",
		"--bitrate-max", "2000",
		"--tolerance", "200",
		"--max-iter", "5",
		"--output", outJSON,
		"--format", "json",
		"--work-dir", dir,
	).CombinedOutput()
	if runErr != nil {
		t.Fatalf("vmafx-tune-go bisect: %v\n%s", runErr, runOut)
	}

	data, readErr := os.ReadFile(outJSON)
	if readErr != nil {
		t.Fatalf("read output JSON: %v", readErr)
	}

	var payload map[string]any
	if parseErr := json.Unmarshal(data, &payload); parseErr != nil {
		t.Fatalf("parse output JSON: %v\n%s", parseErr, string(data))
	}

	for _, key := range []string{
		"schema_version", "src", "encoder", "target_vmaf",
		"bitrate_min_kbps", "bitrate_max_kbps", "tolerance_kbps",
		"tool_version", "wall_time_ms",
		"best_bitrate_kbps", "best_vmaf_score", "converged",
		"iterations", "samples",
	} {
		if _, ok := payload[key]; !ok {
			t.Errorf("output JSON missing required key %q", key)
		}
	}

	if v, ok := payload["schema_version"].(float64); !ok || int(v) != 1 {
		t.Errorf("schema_version = %v, want 1", payload["schema_version"])
	}

	t.Logf("bisect output: %s", string(data))
}
