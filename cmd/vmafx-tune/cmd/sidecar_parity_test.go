// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/sidecar_parity_test.go — replays the operator sequence
// recorded from the Python `vmaf-tune sidecar` CLI (testdata/sidecar/regen.sh)
// through the Go command tree and requires byte-identical stdout,
// byte-identical state.json snapshots, and identical exit statuses.
//
// The fixtures are the contract: stdout (JSON and text form) and the on-disk
// state are what operators and downstream tooling consume. Diagnostics on
// stderr are deliberately NOT pinned byte-for-byte (cobra and argparse phrase
// them differently); only the batch-record skip-line numbers are checked.

package cmd

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// sidecarParityStep is one CLI invocation of the recorded sequence. The
// fixture files are <name>.out (stdout) and <name>.exit (status); snapshot
// names the state.json fixture to compare after the step, when any.
type sidecarParityStep struct {
	name      string
	args      []string
	snapshot  string
	skipLines []int // batch-record: the 1-based lines Python reported as skipped
}

// sidecarParitySequence mirrors regen.sh step for step. Order matters: the
// state accumulates, so a later step's expected bytes depend on every
// earlier one.
var sidecarParitySequence = []sidecarParityStep{
	{name: "status_cold_json", args: []string{"status", "--cache-dir", "cache", "--json"}},
	{name: "status_cold_text", args: []string{"status", "--cache-dir", "cache"}},
	{name: "predict_cold_json", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26", "--json"}},
	{name: "record_1_json", args: []string{"record", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26", "--observed-vmaf", "91.75", "--json"},
		snapshot: "state_after_record_1.json"},
	{name: "record_2_text", args: []string{"record", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "30", "--observed-vmaf", "88.5"},
		snapshot: "state_after_record_2.json"},
	{name: "record_3_int_json", args: []string{"record", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "22", "--observed-vmaf", "96", "--json"},
		snapshot: "state_after_record_3.json"},
	{name: "batch_1_json", args: []string{"batch-record", "--cache-dir", "cache",
		"--captures-jsonl", "captures.jsonl", "--json"},
		snapshot: "state_after_batch_1.json", skipLines: []int{4, 5, 6}},
	{name: "batch_2_text", args: []string{"batch-record", "--cache-dir", "cache",
		"--captures-jsonl", "captures.jsonl"},
		snapshot: "state_after_batch_2.json", skipLines: []int{4, 5, 6}},
	{name: "status_warm_json", args: []string{"status", "--cache-dir", "cache", "--json"}},
	{name: "status_warm_text", args: []string{"status", "--cache-dir", "cache"}},
	{name: "predict_warm_json", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26", "--json"}},
	{name: "predict_warm_text", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26"}},
	{name: "record_nopersist_json", args: []string{"record", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26", "--observed-vmaf", "91.75",
		"--no-persist", "--json"},
		snapshot: "state_after_nopersist.json"},
	{name: "status_x265_json", args: []string{"status", "--cache-dir", "cache",
		"--codec", "libx265", "--predictor-version", "predictor_v2", "--json"}},
	// Error paths: Python returns 2 from every one and prints nothing on stdout.
	{name: "err_bad_codec", args: []string{"status", "--cache-dir", "cache", "--codec", "libnope"}},
	{name: "err_missing_required", args: []string{"record", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26"}},
	{name: "err_bad_crf", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "features.json", "--crf", "26.5"}},
	{name: "err_unknown_flag", args: []string{"status", "--cache-dir", "cache", "--bogus"}},
	{name: "err_features_missing", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "absent.json", "--crf", "26"}},
	{name: "err_features_array", args: []string{"record", "--cache-dir", "cache",
		"--features-json", "array.json", "--crf", "26", "--observed-vmaf", "90"}},
	{name: "err_features_partial", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "partial.json", "--crf", "26"}},
	{name: "err_features_broken", args: []string{"predict", "--cache-dir", "cache",
		"--features-json", "broken-features.txt", "--crf", "26"}},
	{name: "err_captures_missing", args: []string{"batch-record", "--cache-dir", "cache",
		"--captures-jsonl", "absent.jsonl"}},
}

var skipLinePattern = regexp.MustCompile(`skip line (\d+):`)

// TestSidecarPythonParity is the byte-compatibility gate for the sidecar
// group. It runs in a scratch working directory with the same relative
// --cache-dir and pinned host-uuid the fixture generator used, so every
// state_path and every payload renders identically.
func TestSidecarPythonParity(t *testing.T) {
	fixtures, err := filepath.Abs(filepath.Join("testdata", "sidecar"))
	if err != nil {
		t.Fatalf("resolve fixture dir: %v", err)
	}
	work := t.TempDir()
	t.Chdir(work)
	t.Setenv("XDG_CACHE_HOME", filepath.Join(work, "xdg"))

	for _, name := range []string{"features.json", "captures.jsonl", "array.json",
		"partial.json", "broken-features.txt"} {
		copyFixtureFile(t, filepath.Join(fixtures, name), filepath.Join(work, name))
	}
	if err := os.Mkdir(filepath.Join(work, "cache"), 0o750); err != nil {
		t.Fatalf("mkdir cache: %v", err)
	}
	copyFixtureFile(t, filepath.Join(fixtures, "host-uuid"),
		filepath.Join(work, "cache", "host-uuid"))

	statePath := filepath.Join(work, "cache", "predictor_v1", "libx264", "state.json")
	for _, step := range sidecarParitySequence {
		t.Run(step.name, func(t *testing.T) {
			wantOut := readFixture(t, fixtures, step.name+".out")
			wantExit, err := strconv.Atoi(strings.TrimSpace(
				string(readFixture(t, fixtures, step.name+".exit"))))
			if err != nil {
				t.Fatalf("parse %s.exit: %v", step.name, err)
			}

			var runErr error
			stdout, stderr := captureStdio(t, func() {
				root := newRoot("dev")
				root.Cobra().SetArgs(append([]string{"sidecar"}, step.args...))
				root.Cobra().SetOut(io.Discard)
				root.Cobra().SetErr(io.Discard)
				runErr = root.Execute()
			})

			if got := exitCodeOf(runErr); got != wantExit {
				t.Errorf("exit status = %d, want %d (err=%v)", got, wantExit, runErr)
			}
			if stdout != string(wantOut) {
				t.Errorf("stdout differs from the Python fixture:\n%s",
					firstDifference(string(wantOut), stdout))
			}
			if step.snapshot != "" {
				want := readFixture(t, fixtures, step.snapshot)
				got, err := os.ReadFile(statePath) // #nosec G304 -- test scratch dir
				if err != nil {
					t.Fatalf("read %s: %v", statePath, err)
				}
				if !bytes.Equal(got, want) {
					t.Errorf("state.json differs from %s:\n%s", step.snapshot,
						firstDifference(string(want), string(got)))
				}
			}
			if step.skipLines != nil {
				var got []int
				for _, m := range skipLinePattern.FindAllStringSubmatch(stderr, -1) {
					n, err := strconv.Atoi(m[1])
					if err != nil {
						t.Fatalf("parse skip line %q: %v", m[1], err)
					}
					got = append(got, n)
				}
				if fmt.Sprint(got) != fmt.Sprint(step.skipLines) {
					t.Errorf("skipped lines = %v, want %v\nstderr:\n%s",
						got, step.skipLines, stderr)
				}
			}
		})
	}

	// The state directory holds exactly what Python leaves behind: the state
	// file, and no stray temp file from the atomic rename.
	entries, err := os.ReadDir(filepath.Dir(statePath))
	if err != nil {
		t.Fatalf("read state dir: %v", err)
	}
	var listing strings.Builder
	for _, e := range entries {
		listing.WriteString(e.Name())
		listing.WriteString("\n")
	}
	if want := string(readFixture(t, fixtures, "state_dir_listing.txt")); listing.String() != want {
		t.Errorf("state dir listing = %q, want %q", listing.String(), want)
	}
}

// TestSidecarModelUnresolvableExitsUsage pins the --model contract the
// fixtures cannot carry: it depends on the Python host having onnxruntime
// importable (Predictor() then raises FileNotFoundError and the CLI returns
// 2; without onnxruntime the flag is silently ignored). The Go binary
// resolves the name through the model registry and reports a miss as a
// usage failure, matching the onnxruntime-installed Python behaviour.
func TestSidecarModelUnresolvableExitsUsage(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	t.Setenv("VMAFX_MODEL_DIR", t.TempDir())

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{"sidecar", "status", "--cache-dir", t.TempDir(),
		"--model", filepath.Join(t.TempDir(), "predictor_libx264.onnx")})
	root.Cobra().SetOut(io.Discard)
	root.Cobra().SetErr(io.Discard)
	err := root.Execute()
	if err == nil {
		t.Fatal("an unresolvable --model must fail")
	}
	if got := exitCodeOf(err); got != usageExitCode {
		t.Errorf("exit status = %d, want %d (err=%v)", got, usageExitCode, err)
	}
}

// TestSplitLinesUniversal pins the CPython newline=None rule the batch reader
// relies on for its skip-line numbering.
func TestSplitLinesUniversal(t *testing.T) {
	t.Parallel()
	tests := []struct {
		in   string
		want []string
	}{
		{"", nil},
		{"a", []string{"a"}},
		{"a\n", []string{"a"}},
		{"a\nb", []string{"a", "b"}},
		{"a\r\nb\r\n", []string{"a", "b"}},
		{"a\rb\r", []string{"a", "b"}},
		{"\n\nc", []string{"", "", "c"}},
		{"a\r\n\r\nb", []string{"a", "", "b"}},
	}
	for _, tc := range tests {
		got := splitLinesUniversal(tc.in)
		if fmt.Sprint(got) != fmt.Sprint(tc.want) {
			t.Errorf("splitLinesUniversal(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

// TestIntFieldMirrorsPythonInt covers int(row["crf"]) semantics: a float
// truncates, a string must be an integer literal, anything else is an error.
func TestIntFieldMirrorsPythonInt(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name    string
		row     map[string]any
		want    int
		wantErr bool
	}{
		{"number", map[string]any{"crf": 28.0}, 28, false},
		{"fractional number truncates", map[string]any{"crf": 28.7}, 28, false},
		{"integer string", map[string]any{"crf": " 28 "}, 28, false},
		{"fractional string is rejected", map[string]any{"crf": "28.5"}, 0, true},
		{"missing", map[string]any{}, 0, true},
		{"boolean", map[string]any{"crf": true}, 0, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := intField(tc.row, "crf")
			if (err != nil) != tc.wantErr {
				t.Fatalf("err = %v, wantErr %v", err, tc.wantErr)
			}
			if got != tc.want {
				t.Errorf("intField = %d, want %d", got, tc.want)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

func readFixture(t *testing.T, dir, name string) []byte {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(dir, name)) // #nosec G304 -- test fixture
	if err != nil {
		t.Fatalf("read fixture %s: %v (run testdata/sidecar/regen.sh)", name, err)
	}
	return data
}

func copyFixtureFile(t *testing.T, src, dst string) {
	t.Helper()
	data, err := os.ReadFile(src) // #nosec G304 -- test fixture
	if err != nil {
		t.Fatalf("read %s: %v", src, err)
	}
	if err := os.WriteFile(dst, data, 0o600); err != nil {
		t.Fatalf("write %s: %v", dst, err)
	}
}

// firstDifference renders the first line on which want and got disagree.
func firstDifference(want, got string) string {
	wantLines := strings.Split(want, "\n")
	gotLines := strings.Split(got, "\n")
	for i := 0; i < len(wantLines) || i < len(gotLines); i++ {
		var w, g string
		if i < len(wantLines) {
			w = wantLines[i]
		}
		if i < len(gotLines) {
			g = gotLines[i]
		}
		if w != g {
			return fmt.Sprintf("line %d:\n  want: %q\n  got:  %q", i+1, w, g)
		}
	}
	return "(no line differs; trailing bytes?)"
}

// captureStdio runs fn with os.Stdout and os.Stderr redirected to pipes and
// returns what was written to each. The sidecar handlers print through
// fmt.Print* / os.Stderr by design (the Python surface writes to the real
// streams and operators pipe stdout into jq), so the test intercepts the file
// descriptors rather than cobra's writers.
func captureStdio(t *testing.T, fn func()) (stdout, stderr string) {
	t.Helper()
	restoreOut := redirectStream(t, &os.Stdout)
	restoreErr := redirectStream(t, &os.Stderr)
	fn()
	return restoreOut(), restoreErr()
}

// redirectStream points *target at a pipe and returns a function that
// restores the original and yields everything written in between.
func redirectStream(t *testing.T, target **os.File) func() string {
	t.Helper()
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatalf("pipe: %v", err)
	}
	original := *target
	*target = writer
	done := make(chan string, 1)
	go func() {
		data, readErr := io.ReadAll(reader)
		if readErr != nil {
			data = append(data, []byte("\n<read error: "+readErr.Error()+">")...)
		}
		done <- string(data)
	}()
	return func() string {
		*target = original
		if err := writer.Close(); err != nil {
			t.Errorf("close pipe writer: %v", err)
		}
		out := <-done
		if err := reader.Close(); err != nil {
			t.Errorf("close pipe reader: %v", err)
		}
		return out
	}
}
