// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/encodeprofile_test.go — unit tests for the
// "encode-profile" subcommand.
//
// Tests cover:
//   - Flag surface matches `vmaf-tune encode-profile` name-for-name.
//   - --dry-run emits the exact JSON payload shape, with the argv the profile
//     implies.
//   - Selection flags (--codec, --target-vmaf, --recommendation-index) and
//     override flags reach the request.
//   - "Flag not passed" is distinguished from "flag passed as zero", which is
//     what --duration 0 and --recommendation-index 0 depend on.
//   - Sad paths: missing profile, unusable filters, unknown --source-kind.
//   - exitCodeOf maps errors onto the process exit status.
//
// ADR-0770: vmafx-tune Stage 4 — encode-profile subcommand.

package cmd

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// profileJSON builds a minimal report payload carrying an encoder_profile
// block with three recommendations.
func profileJSON(t *testing.T) []byte {
	t.Helper()
	rec := func(codec string, target float64, crf int, bitrate float64, pareto bool) map[string]any {
		return map[string]any{
			"codec": codec, "target_vmaf": target, "crf": crf, "quality": crf,
			"quality_knob": "crf", "preset": "medium", "bitrate_kbps": bitrate,
			"achieved_vmaf": target + 0.4, "encode_time_ms": 1234.5,
			"selected_pareto": pareto, "source_row": "sweep",
			"encoder_version": codec + "-x",
		}
	}
	payload := map[string]any{
		"schema_version": 2,
		"encoder_profile": map[string]any{
			"schema":         "vmaftune.encoder_profile.v1",
			"schema_version": 1,
			"source": map[string]any{
				"path": "clips/ref.yuv", "width": 320, "height": 240,
				"fps": 30.0, "duration_s": 10.0,
			},
			"run": map[string]any{
				"preset": "medium", "pix_fmt": "yuv420p", "ffmpeg_bin": "ffmpeg",
			},
			"recommendations": []any{
				rec("libx265", 92.0, 28, 2500.0, true),
				rec("libx264", 92.0, 26, 3100.5, true),
				rec("libx264", 90.0, 30, 2000.0, false),
			},
		},
	}
	b, err := json.Marshal(payload)
	if err != nil {
		t.Fatalf("marshal profile: %v", err)
	}
	return b
}

// writeProfile writes the profile fixture and returns its path.
func writeProfile(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "report.json")
	if err := os.WriteFile(path, profileJSON(t), 0o600); err != nil {
		t.Fatalf("write profile: %v", err)
	}
	return path
}

// dryRunPayload is the parsed --dry-run JSON.
type dryRunPayload struct {
	OK      bool           `json:"ok"`
	DryRun  bool           `json:"dry_run"`
	Profile string         `json:"profile"`
	Output  string         `json:"output"`
	Argv    []string       `json:"ffmpeg_argv"`
	Sel     map[string]any `json:"selected"`
}

// runDryRun executes the subcommand with --dry-run and returns the parsed
// payload.
//
// The command writes its result through the package-level resultWriter rather
// than to cobra's output writer, mirroring the Python CLI's sys.stdout.write.
// Swapping that one variable is enough to capture it; redirecting the process
// os.Stdout would be both fragile and needless.
func runDryRun(t *testing.T, args ...string) dryRunPayload {
	t.Helper()

	var buf bytes.Buffer
	restore := swapResultWriter(&buf)
	defer restore()

	root := newRoot("dev")
	root.Cobra().SetArgs(args)
	if err := root.Execute(); err != nil {
		t.Fatalf("execute encode-profile: %v (stdout: %s)", err, buf.String())
	}

	var payload dryRunPayload
	if err := json.Unmarshal(buf.Bytes(), &payload); err != nil {
		t.Fatalf("parse dry-run JSON: %v\nstdout: %s", err, buf.String())
	}
	return payload
}

// swapResultWriter redirects the subcommand's result stream and returns a
// function that restores it.
func swapResultWriter(w io.Writer) func() {
	orig := resultWriter
	resultWriter = w
	return func() { resultWriter = orig }
}

// TestEncodeProfileFlagSurface pins the flag names against the Python CLI.
func TestEncodeProfileFlagSurface(t *testing.T) {
	t.Parallel()

	cmd := newEncodeProfileCmd()

	for _, name := range []string{
		"profile", "output", "src", "codec", "target-vmaf", "recommendation-index",
		"preset", "pix-fmt", "framerate", "width", "height", "duration",
		"source-kind", "sample-clip-seconds", "sample-clip-start-s",
		"extra-ffmpeg-arg", "ffmpeg-bin", "dry-run",
	} {
		if cmd.Flags().Lookup(name) == nil {
			t.Errorf("--%s is missing", name)
		}
	}

	if got := cmd.Flags().Lookup("source-kind").DefValue; got != "auto" {
		t.Errorf("--source-kind default = %q, want %q", got, "auto")
	}
}

// TestEncodeProfileDryRun checks the payload shape and the default selection:
// with no filters, the lowest-bitrate Pareto row (libx265 @ 2500) wins.
func TestEncodeProfileDryRun(t *testing.T) {
	profile := writeProfile(t)
	out := filepath.Join(t.TempDir(), "encoded.mkv")

	got := runDryRun(t, "encode-profile", "--profile", profile, "--output", out, "--dry-run")

	if !got.OK || !got.DryRun {
		t.Errorf("ok=%v dry_run=%v, want both true", got.OK, got.DryRun)
	}
	if got.Profile != profile {
		t.Errorf("profile = %q, want %q", got.Profile, profile)
	}
	if got.Output != out {
		t.Errorf("output = %q, want %q", got.Output, out)
	}
	if codec, _ := got.Sel["codec"].(string); codec != "libx265" {
		t.Errorf("selected codec = %v, want libx265 (cheapest Pareto row)", got.Sel["codec"])
	}

	// The source is a .yuv, so the argv must declare the raw geometry, and the
	// framerate must keep CPython's trailing ".0".
	want := []string{
		"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
		"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "320x240", "-r", "30.0",
		"-t", "10.0",
		"-i", "clips/ref.yuv",
		"-c:v", "libx265", "-preset", "medium", "-crf", "28",
		out,
	}
	if strings.Join(got.Argv, " ") != strings.Join(want, " ") {
		t.Errorf("ffmpeg_argv =\n %v\nwant\n %v", got.Argv, want)
	}
}

// TestEncodeProfileSelectionFlags checks that each selection flag narrows the
// candidate set the way the Python CLI does.
func TestEncodeProfileSelectionFlags(t *testing.T) {
	profile := writeProfile(t)
	out := filepath.Join(t.TempDir(), "encoded.mkv")

	tests := []struct {
		name       string
		extra      []string
		wantCodec  string
		wantCRF    float64
		wantTarget float64
	}{
		{
			name:      "no filters picks the cheapest pareto row",
			wantCodec: "libx265", wantCRF: 28, wantTarget: 92,
		},
		{
			name:      "codec filter",
			extra:     []string{"--codec", "libx264"},
			wantCodec: "libx264", wantCRF: 26, wantTarget: 92,
		},
		{
			name:      "target filter",
			extra:     []string{"--target-vmaf", "90"},
			wantCodec: "libx264", wantCRF: 30, wantTarget: 90,
		},
		{
			// Index 0 is only distinguishable from "flag absent" because the
			// command consults Flags().Changed.
			name:      "explicit index zero",
			extra:     []string{"--recommendation-index", "0"},
			wantCodec: "libx265", wantCRF: 28, wantTarget: 92,
		},
		{
			// Index 2 reaches the non-Pareto row, which sorts last.
			name:      "index two reaches the non-pareto row",
			extra:     []string{"--recommendation-index", "2"},
			wantCodec: "libx264", wantCRF: 30, wantTarget: 90,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			args := append([]string{
				"encode-profile", "--profile", profile, "--output", out, "--dry-run",
			}, tt.extra...)
			got := runDryRun(t, args...)

			if codec, _ := got.Sel["codec"].(string); codec != tt.wantCodec {
				t.Errorf("codec = %v, want %v", got.Sel["codec"], tt.wantCodec)
			}
			if crf, _ := got.Sel["crf"].(float64); crf != tt.wantCRF {
				t.Errorf("crf = %v, want %v", got.Sel["crf"], tt.wantCRF)
			}
			if target, _ := got.Sel["target_vmaf"].(float64); target != tt.wantTarget {
				t.Errorf("target_vmaf = %v, want %v", got.Sel["target_vmaf"], tt.wantTarget)
			}
		})
	}
}

// TestEncodeProfileOverrideFlags checks that the override flags reach the argv.
func TestEncodeProfileOverrideFlags(t *testing.T) {
	profile := writeProfile(t)
	out := filepath.Join(t.TempDir(), "encoded.mkv")

	tests := []struct {
		name     string
		extra    []string
		wantArgs []string
		absent   []string
	}{
		{
			name:     "preset override",
			extra:    []string{"--preset", "veryslow"},
			wantArgs: []string{"-preset", "veryslow"},
		},
		{
			name:     "geometry and pix-fmt overrides",
			extra:    []string{"--width", "1280", "--height", "720", "--pix-fmt", "yuv420p10le"},
			wantArgs: []string{"-s", "1280x720", "-pix_fmt", "yuv420p10le"},
		},
		{
			name:     "framerate override keeps the python float rendering",
			extra:    []string{"--framerate", "24"},
			wantArgs: []string{"-r", "24.0"},
		},
		{
			name:     "source override",
			extra:    []string{"--src", "other/clip.yuv"},
			wantArgs: []string{"-i", "other/clip.yuv"},
		},
		{
			name: "sample clip replaces the bound duration",
			extra: []string{
				"--sample-clip-seconds", "2.5", "--sample-clip-start-s", "3.75",
			},
			wantArgs: []string{"-ss", "3.75", "-t", "2.5"},
		},
		{
			// --duration 0 must suppress the profile's own -t, which only
			// works because "passed as zero" is distinguished from "absent".
			name:   "explicit zero duration suppresses -t",
			extra:  []string{"--duration", "0"},
			absent: []string{"-t"},
		},
		{
			name:     "extra ffmpeg args land after the codec args",
			extra:    []string{"--extra-ffmpeg-arg=-movflags", "--extra-ffmpeg-arg=+faststart"},
			wantArgs: []string{"-movflags", "+faststart"},
		},
		{
			// --source-kind container drops the raw-video input flags even
			// though the path still ends in .yuv.
			name:   "source kind container drops the raw flags",
			extra:  []string{"--source-kind", "container"},
			absent: []string{"-f", "rawvideo"},
		},
		{
			name:     "ffmpeg-bin override",
			extra:    []string{"--ffmpeg-bin", "/opt/ffmpeg/bin/ffmpeg"},
			wantArgs: []string{"/opt/ffmpeg/bin/ffmpeg"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			args := append([]string{
				"encode-profile", "--profile", profile, "--output", out, "--dry-run",
			}, tt.extra...)
			got := runDryRun(t, args...)

			joined := " " + strings.Join(got.Argv, " ") + " "
			for _, want := range tt.wantArgs {
				if !strings.Contains(joined, " "+want+" ") {
					t.Errorf("argv missing %q; got %v", want, got.Argv)
				}
			}
			for _, absent := range tt.absent {
				if strings.Contains(joined, " "+absent+" ") {
					t.Errorf("argv should not contain %q; got %v", absent, got.Argv)
				}
			}
		})
	}
}

func TestEncodeProfileErrors(t *testing.T) {
	profile := writeProfile(t)
	out := filepath.Join(t.TempDir(), "encoded.mkv")

	tests := []struct {
		name    string
		args    []string
		wantSub string
	}{
		{
			name:    "missing profile file",
			args:    []string{"--profile", "/nonexistent/report.json", "--output", out},
			wantSub: "cannot read profile",
		},
		{
			name:    "codec filter matches nothing",
			args:    []string{"--profile", profile, "--output", out, "--codec", "libtheora"},
			wantSub: "no recommendation matching",
		},
		{
			name:    "target filter matches nothing",
			args:    []string{"--profile", profile, "--output", out, "--target-vmaf", "42"},
			wantSub: "no recommendation matching",
		},
		{
			name: "index past the filtered end",
			args: []string{
				"--profile", profile, "--output", out, "--recommendation-index", "99",
			},
			wantSub: "outside filtered range",
		},
		{
			name: "unknown source kind",
			args: []string{
				"--profile", profile, "--output", out, "--source-kind", "sideways",
			},
			wantSub: "unknown source kind",
		},
		{
			name:    "missing required --profile",
			args:    []string{"--output", out},
			wantSub: "profile",
		},
		{
			name:    "missing required --output",
			args:    []string{"--profile", profile},
			wantSub: "output",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			root := newRoot("dev")
			root.Cobra().SetArgs(append([]string{"encode-profile"}, tt.args...))
			root.Cobra().SetOut(io.Discard)
			root.Cobra().SetErr(io.Discard)
			err := root.Execute()
			if err == nil {
				t.Fatal("expected an error, got nil")
			}
			if !strings.Contains(err.Error(), tt.wantSub) {
				t.Errorf("error %q does not contain %q", err, tt.wantSub)
			}
		})
	}
}

// TestExitCodeOf pins the exit-status mapping. encode-profile relies on it to
// propagate FFmpeg's own status, the way the Python CLI returns
// int(result.exit_status).
func TestExitCodeOf(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		err  error
		want int
	}{
		{"nil error is success", nil, 0},
		{"plain error is 1", errors.New("boom"), 1},
		{
			name: "carried status wins",
			err:  exitCodeError{code: 234, err: errors.New("ffmpeg exited with status 234")},
			want: 234,
		},
		{
			// A zero carried status would mean "success" on a path that only
			// runs for failures, so it falls back to 1.
			name: "zero carried status falls back to 1",
			err:  exitCodeError{code: 0, err: errors.New("boom")},
			want: 1,
		},
		{
			name: "wrapped carried status is still found",
			err: fmt.Errorf("encode: %w",
				exitCodeError{code: 69, err: errors.New("boom")}),
			want: 69,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			if got := exitCodeOf(tt.err); got != tt.want {
				t.Errorf("exitCodeOf(%v) = %d, want %d", tt.err, got, tt.want)
			}
		})
	}
}

// TestUsageErrorsExitTwo pins the exit-status contract against the Python CLI.
//
// `vmaf-tune` returns 2 for every validation failure (each subcommand writes a
// diagnostic and `return 2`) and argparse exits 2 for a flag-layer failure.
// Cobra's default is 1 for both, and the flag layer runs before RunE — hence
// useUsageExitCode plus the in-RunE required-flag checks.
func TestUsageErrorsExitTwo(t *testing.T) {
	profile := writeProfile(t)
	corpus := writeCorpus(t, corpusJSONL)

	tests := []struct {
		name string
		args []string
	}{
		// Domain validation, raised inside RunE.
		{"benchmark missing corpus file", []string{
			"benchmark", "--from-corpus", "/nonexistent.jsonl",
		}},
		{"benchmark absent baseline", []string{
			"benchmark", "--from-corpus", corpus, "--baseline-encoder", "libtheora",
		}},
		{"encode-profile missing file", []string{
			"encode-profile", "--profile", "/nonexistent.json", "--output", "o.mkv", "--dry-run",
		}},
		{"encode-profile filter matches nothing", []string{
			"encode-profile", "--profile", profile, "--output", "o.mkv",
			"--codec", "libtheora", "--dry-run",
		}},
		// Flag layer, raised by cobra before RunE.
		{"benchmark missing required flag", []string{"benchmark"}},
		{"benchmark unknown flag", []string{
			"benchmark", "--from-corpus", corpus, "--nope",
		}},
		{"benchmark unparseable float", []string{
			"benchmark", "--from-corpus", corpus, "--target-vmaf", "notanumber",
		}},
		{"encode-profile missing required flags", []string{"encode-profile"}},
		{"encode-profile missing output", []string{"encode-profile", "--profile", profile}},
		{"encode-profile unparseable int", []string{
			"encode-profile", "--profile", profile, "--output", "o.mkv", "--width", "notanint",
		}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			root := newRoot("dev")
			root.Cobra().SetArgs(tt.args)
			root.Cobra().SetOut(io.Discard)
			root.Cobra().SetErr(io.Discard)

			err := root.Execute()
			if err == nil {
				t.Fatal("expected an error, got nil")
			}
			if got := exitCodeOf(err); got != 2 {
				t.Errorf("exit code = %d, want 2 (matching the Python CLI); err = %v", got, err)
			}
		})
	}
}

// TestExitCodeErrorUnwraps checks that errors.Is / errors.As see through the
// wrapper, so callers can still match on the underlying error.
func TestExitCodeErrorUnwraps(t *testing.T) {
	t.Parallel()

	inner := errors.New("ffmpeg died")
	err := error(exitCodeError{code: 3, err: inner})

	if !errors.Is(err, inner) {
		t.Error("errors.Is cannot see the wrapped error")
	}
	if err.Error() != "ffmpeg died" {
		t.Errorf("Error() = %q, want %q", err.Error(), "ffmpeg died")
	}
}
