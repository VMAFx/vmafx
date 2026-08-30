// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// End-to-end tests for the tune-per-shot subcommand, driving the built
// binary the way an operator would.

package cmd_test

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// buildPerShotBinary builds vmafx-tune-go into a temp dir and returns its path.
func buildPerShotBinary(t *testing.T) string {
	t.Helper()
	binPath := filepath.Join(t.TempDir(), "vmafx-tune-go")
	out, err := exec.Command(
		"go", "build",
		"-o", binPath,
		"github.com/VMAFx/vmafx/cmd/vmafx-tune",
	).CombinedOutput()
	if err != nil {
		t.Fatalf("build vmafx-tune-go: %v\n%s", err, out)
	}
	return binPath
}

// TestPerShot_helpFlag verifies the subcommand exposes the Python parser's
// flag names, which is what makes an existing runbook portable.
func TestPerShot_helpFlag(t *testing.T) {
	t.Parallel()
	binPath := buildPerShotBinary(t)

	out, err := exec.Command(binPath, "tune-per-shot", "--help").CombinedOutput()
	if err != nil {
		t.Fatalf("tune-per-shot --help exited non-zero: %v\n%s", err, out)
	}
	helpText := string(out)
	for _, want := range []string{
		"--src", "--width", "--height", "--pix-fmt", "--framerate",
		"--target-vmaf", "--encoder", "--bitdepth", "--total-frames",
		"--scene-threshold", "--max-shot-duration", "--per-shot-bin",
		"--ffmpeg-bin", "--vmaf-bin", "--preset", "--crf-min", "--crf-max",
		"--max-iterations", "--vmaf-model", "--neg", "--fast-nr",
		"--score-backend", "--predicate-module", "--output", "--segment-dir",
		"--plan-out", "--script-out", "--workdir", "--max-concurrent-decodes",
	} {
		if !strings.Contains(helpText, want) {
			t.Errorf("tune-per-shot --help missing flag %q", want)
		}
	}
}

// TestPerShot_isNoLongerAStub verifies the subcommand runs its own logic
// rather than the "not yet ported" redirect.
func TestPerShot_isNoLongerAStub(t *testing.T) {
	t.Parallel()
	binPath := buildPerShotBinary(t)

	out, _ := exec.Command(binPath, "tune-per-shot", "--help").CombinedOutput()
	if strings.Contains(string(out), "not yet ported") {
		t.Errorf("tune-per-shot still advertises itself as a stub:\n%s", out)
	}
	rootOut, _ := exec.Command(binPath, "--help").CombinedOutput()
	if !strings.Contains(string(rootOut), "tune-per-shot") {
		t.Errorf("root help does not list tune-per-shot:\n%s", rootOut)
	}
}

// TestPerShot_missingSource verifies a non-existent --src exits non-zero.
func TestPerShot_missingSource(t *testing.T) {
	t.Parallel()
	binPath := buildPerShotBinary(t)

	out, err := exec.Command(binPath, "tune-per-shot",
		"--src", "/nonexistent/path/src.mp4",
	).CombinedOutput()
	if err == nil {
		t.Fatalf("expected non-zero exit for a missing source, got success:\n%s", out)
	}
}

// TestPerShot_rejectsUnportedFlags verifies the two flags with no Go
// implementation fail loudly and point at the Python binary, rather than
// being silently accepted and ignored.
func TestPerShot_rejectsUnportedFlags(t *testing.T) {
	t.Parallel()
	binPath := buildPerShotBinary(t)
	src := filepath.Join(t.TempDir(), "clip.mp4")
	if err := os.WriteFile(src, []byte("not a real video"), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}

	cases := []struct {
		name string
		args []string
		want string
	}{
		{
			name: "predicate-module",
			args: []string{"--predicate-module", "mymod:pick"},
			want: "vmaf-tune tune-per-shot --predicate-module",
		},
		{
			name: "fast-nr",
			args: []string{"--fast-nr"},
			want: "onnxruntime",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			argv := append([]string{"tune-per-shot", "--src", src}, tc.args...)
			out, err := exec.Command(binPath, argv...).CombinedOutput()
			if err == nil {
				t.Fatalf("expected non-zero exit for %s, got success:\n%s", tc.name, out)
			}
			if !strings.Contains(string(out), tc.want) {
				t.Errorf("error output should mention %q, got:\n%s", tc.want, out)
			}
		})
	}
}

// TestPerShot_rejectsBadFlagValues covers the argument-validation branches
// that must fail before any encode is attempted.
func TestPerShot_rejectsBadFlagValues(t *testing.T) {
	t.Parallel()
	binPath := buildPerShotBinary(t)
	dir := t.TempDir()
	src := filepath.Join(dir, "clip.yuv")
	if err := os.WriteFile(src, []byte("raw"), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}

	cases := []struct {
		name string
		args []string
		want string
	}{
		{"target out of range", []string{"--target-vmaf", "150"}, "out of range"},
		{"zero iterations", []string{"--max-iterations", "0"}, "must be positive"},
		{"zero decodes", []string{"--max-concurrent-decodes", "0"}, "must be >= 1"},
		{"bad bitdepth", []string{"--bitdepth", "9"}, "must be 8, 10 or 12"},
		{"unported codec", []string{"--encoder", "libvvenc"}, "Python vmaf-tune"},
		{"bad preset", []string{"--encoder", "libx264", "--preset", "nope"}, "not a libx264 preset"},
		{"half a CRF range", []string{"--crf-min", "20"}, "pass both"},
		{"inverted CRF range", []string{"--crf-min", "40", "--crf-max", "20"}, "invalid CRF range"},
		{"raw source without geometry", nil, "required for raw YUV"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			argv := append([]string{
				"tune-per-shot", "--src", src, "--width", "320", "--height", "240",
			}, tc.args...)
			if tc.name == "raw source without geometry" {
				argv = []string{"tune-per-shot", "--src", src}
			}
			out, err := exec.Command(binPath, argv...).CombinedOutput()
			if err == nil {
				t.Fatalf("expected non-zero exit, got success:\n%s", out)
			}
			if !strings.Contains(string(out), tc.want) {
				t.Errorf("error output should mention %q, got:\n%s", tc.want, out)
			}
		})
	}
}

// TestPerShot_planSchemaWithStubPredicateBinaries runs the whole subcommand
// against stub ffmpeg / ffprobe / vmaf / vmaf-perShot binaries, so the plan
// pipeline is exercised end to end without a real encoder or GPU.
//
// The stubs are shell scripts placed on a private PATH:
//   - vmaf-perShot writes a two-shot JSON plan to its --output path,
//   - ffmpeg creates whatever output path it was given (the bisect's encode
//     and the shot extraction both just need a file to exist),
//   - ffprobe reports a fixed bitrate,
//   - vmaf writes a JSON score above any reachable target so the bisect
//     converges on the first probe.
func TestPerShot_planSchemaWithStubPredicateBinaries(t *testing.T) {
	t.Parallel()
	binPath := buildPerShotBinary(t)
	dir := t.TempDir()
	stubDir := filepath.Join(dir, "stubs")
	if err := os.MkdirAll(stubDir, 0o750); err != nil {
		t.Fatalf("mkdir stubs: %v", err)
	}

	writeStub := func(name, body string) string {
		t.Helper()
		p := filepath.Join(stubDir, name)
		if err := os.WriteFile(p, []byte("#!/bin/sh\n"+body), 0o700); err != nil { //nolint:gosec // test stub must be executable
			t.Fatalf("write stub %s: %v", name, err)
		}
		return p
	}

	// vmaf-perShot: emit two shots covering frames 0..47 (inclusive ends).
	perShotStub := writeStub("vmaf-perShot", `
out=""
while [ $# -gt 0 ]; do
  if [ "$1" = "--output" ]; then out="$2"; fi
  shift
done
printf '{"shots":[{"start_frame":0,"end_frame":23},{"start_frame":24,"end_frame":47}]}' > "$out"
`)

	// ffmpeg: the last argument is always the output path in every argv this
	// command builds, so creating it is enough for both the shot extraction
	// and the bisect encode.
	ffmpegStub := writeStub("ffmpeg", `
for last; do :; done
: > "$last"
`)

	// ffprobe: a fixed bitrate for the encoded segment.
	writeStub("ffprobe", `echo 2000000`)

	// vmaf: a score high enough that the bisect accepts its first probe and
	// keeps walking upward until the window collapses.
	vmafStub := writeStub("vmaf", `
out=""
while [ $# -gt 0 ]; do
  if [ "$1" = "--output" ]; then out="$2"; fi
  shift
done
printf '{"pooled_metrics":{"vmaf":{"mean":99.5}}}' > "$out"
`)

	src := filepath.Join(dir, "clip.yuv")
	if err := os.WriteFile(src, make([]byte, 1024), 0o600); err != nil {
		t.Fatalf("write source: %v", err)
	}
	planOut := filepath.Join(dir, "plan.json")
	scriptOut := filepath.Join(dir, "plan.sh")

	cmd := exec.Command(binPath, "tune-per-shot",
		"--src", src,
		"--width", "64", "--height", "64",
		"--framerate", "24",
		"--target-vmaf", "92",
		"--encoder", "libx264",
		"--max-shot-duration", "0", // keep the detector's own two shots
		"--max-iterations", "2",
		"--crf-min", "20", "--crf-max", "24",
		"--per-shot-bin", perShotStub,
		"--ffmpeg-bin", ffmpegStub,
		"--vmaf-bin", vmafStub,
		"--score-backend", "cpu",
		"--workdir", filepath.Join(dir, "work"),
		"--plan-out", planOut,
		"--script-out", scriptOut,
		"--segment-dir", filepath.Join(dir, "segments"),
		"--output", filepath.Join(dir, "final.mp4"),
	)
	cmd.Env = append(os.Environ(), "PATH="+stubDir+string(os.PathListSeparator)+os.Getenv("PATH"))
	out, runErr := cmd.CombinedOutput()
	if runErr != nil {
		t.Fatalf("tune-per-shot: %v\n%s", runErr, out)
	}

	data, readErr := os.ReadFile(planOut) //nolint:gosec // test-local temp path
	if readErr != nil {
		t.Fatalf("read plan: %v", readErr)
	}
	var payload map[string]any
	if err := json.Unmarshal(data, &payload); err != nil {
		t.Fatalf("parse plan JSON: %v\n%s", err, data)
	}
	for _, key := range []string{
		"concat_command", "encoder", "framerate", "predicate",
		"segment_commands", "shots", "target_vmaf",
	} {
		if _, ok := payload[key]; !ok {
			t.Errorf("plan JSON missing required key %q", key)
		}
	}
	if payload["predicate"] != "bisect" {
		t.Errorf("predicate = %v, want \"bisect\"", payload["predicate"])
	}
	if payload["encoder"] != "libx264" {
		t.Errorf("encoder = %v, want libx264", payload["encoder"])
	}

	shots, ok := payload["shots"].([]any)
	if !ok || len(shots) != 2 {
		t.Fatalf("shots = %v, want two entries", payload["shots"])
	}
	first, _ := shots[0].(map[string]any)
	for _, key := range []string{
		"bitrate_kbps", "crf", "end_frame", "predicted_vmaf", "start_frame",
	} {
		if _, present := first[key]; !present {
			t.Errorf("shot entry missing key %q: %v", key, first)
		}
	}
	// The detector's inclusive end_frame 23 must be normalised to 24.
	if first["end_frame"] != float64(24) {
		t.Errorf("first shot end_frame = %v, want 24 (half-open)", first["end_frame"])
	}
	// The bisect's CRF must land inside the requested window.
	if crf, isNum := first["crf"].(float64); !isNum || crf < 20 || crf > 24 {
		t.Errorf("first shot crf = %v, want it inside [20, 24]", first["crf"])
	}

	segs, ok := payload["segment_commands"].([]any)
	if !ok || len(segs) != 2 {
		t.Fatalf("segment_commands = %v, want two entries", payload["segment_commands"])
	}

	// The shell script and the concat listing must both have landed.
	if _, err := os.Stat(scriptOut); err != nil {
		t.Errorf("--script-out was not written: %v", err)
	}
	listing, err := os.ReadFile(filepath.Join(dir, "segments", "concat.txt")) //nolint:gosec // test-local temp path
	if err != nil {
		t.Fatalf("read concat listing: %v", err)
	}
	if strings.Count(string(listing), "file '") != 2 {
		t.Errorf("concat listing should name two segments, got:\n%s", listing)
	}
}
