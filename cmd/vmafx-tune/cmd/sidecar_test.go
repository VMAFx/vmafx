// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/sidecar_test.go — in-package tests for the "sidecar"
// subcommand group wired in as part of the Stage-2 Python-to-Go port.

package cmd

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/tune/predictor"
)

const featuresJSON = `{
  "probe_bitrate_kbps": 4200.5,
  "probe_i_frame_avg_bytes": 51200.0,
  "probe_p_frame_avg_bytes": 8100.0,
  "probe_b_frame_avg_bytes": 2400.0,
  "saliency_mean": 0.42,
  "saliency_var": 0.11,
  "frame_diff_mean": 3.75,
  "y_avg": 112.5,
  "y_var": 900.25,
  "shot_length_frames": 240,
  "fps": 24.0,
  "width": 1920,
  "height": 1080
}`

func writeFixture(t *testing.T, dir, name, content string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatalf("write %s: %v", name, err)
	}
	return path
}

// TestSidecarSubcommandTree pins the nested command names against the Python
// argparse surface.
func TestSidecarSubcommandTree(t *testing.T) {
	t.Parallel()

	got := []string{}
	for _, c := range newSidecarCmd().Commands() {
		got = append(got, c.Name())
	}
	sort.Strings(got)
	want := []string{"batch-record", "predict", "record", "status"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("sidecar subcommands = %v, want %v", got, want)
	}
}

// TestSidecarCommonFlagSurface pins the shared flags every nested subcommand
// carries, with the Python defaults.
func TestSidecarCommonFlagSurface(t *testing.T) {
	t.Parallel()

	for _, sub := range newSidecarCmd().Commands() {
		t.Run(sub.Name(), func(t *testing.T) {
			t.Parallel()
			for flag, want := range map[string]string{
				"codec":             "libx264",
				"cache-dir":         "",
				"predictor-version": "predictor_v1",
				"model":             "",
				"json":              "false",
			} {
				f := sub.Flags().Lookup(flag)
				if f == nil {
					t.Errorf("--%s is missing from %q", flag, sub.Name())
					continue
				}
				if f.DefValue != want {
					t.Errorf("--%s default = %q, want %q", flag, f.DefValue, want)
				}
			}
		})
	}
}

// TestSidecarStatusJSON drives the real command tree against an isolated cache
// root and checks the machine-readable payload's schema.
func TestSidecarStatusJSON(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	cache := t.TempDir()

	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"sidecar", "status", "--cache-dir", cache, "--json",
		})
		if err := root.Execute(); err != nil {
			t.Fatalf("execute sidecar status: %v", err)
		}
	})

	var payload map[string]any
	if err := json.Unmarshal([]byte(out), &payload); err != nil {
		t.Fatalf("status payload is not JSON: %v\n%s", err, out)
	}
	if payload["schema"] != "vmaf-tune-sidecar-status/v1" {
		t.Errorf("schema = %v", payload["schema"])
	}
	if payload["codec"] != "libx264" {
		t.Errorf("codec = %v, want libx264", payload["codec"])
	}
	if payload["n_updates"] != 0.0 {
		t.Errorf("a fresh sidecar should report 0 updates, got %v", payload["n_updates"])
	}
	statePath, _ := payload["state_path"].(string)
	if !strings.HasPrefix(statePath, cache) {
		t.Errorf("state_path %q should sit under the --cache-dir %q", statePath, cache)
	}
	for _, key := range []string{"host_uuid", "predictor_version", "schema_version",
		"recent_residual_rms"} {
		if _, ok := payload[key]; !ok {
			t.Errorf("status payload is missing %q", key)
		}
	}
}

// TestSidecarRecordThenPredict is the end-to-end operator loop: record one
// observation, then confirm the correction has moved the prediction and that
// the state persisted.
func TestSidecarRecordThenPredict(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	dir := t.TempDir()
	cache := filepath.Join(dir, "cache")
	features := writeFixture(t, dir, "features.json", featuresJSON)

	runJSON := func(args ...string) map[string]any {
		t.Helper()
		out := captureStdout(t, func() {
			root := newRoot("dev")
			root.Cobra().SetArgs(args)
			if err := root.Execute(); err != nil {
				t.Fatalf("execute %v: %v", args, err)
			}
		})
		var payload map[string]any
		if err := json.Unmarshal([]byte(out), &payload); err != nil {
			t.Fatalf("payload is not JSON: %v\n%s", err, out)
		}
		return payload
	}

	before := runJSON("sidecar", "predict", "--cache-dir", cache,
		"--features-json", features, "--crf", "26", "--json")
	if before["correction"] != 0.0 {
		t.Errorf("a cold-start correction must be exactly 0.0, got %v", before["correction"])
	}
	if before["base_vmaf"] != before["sidecar_vmaf"] {
		t.Errorf("cold start: sidecar_vmaf %v should equal base_vmaf %v",
			before["sidecar_vmaf"], before["base_vmaf"])
	}

	recorded := runJSON("sidecar", "record", "--cache-dir", cache,
		"--features-json", features, "--crf", "26", "--observed-vmaf", "91.5", "--json")
	if recorded["schema"] != "vmaf-tune-sidecar-record/v1" {
		t.Errorf("schema = %v", recorded["schema"])
	}
	if recorded["n_updates"] != 1.0 {
		t.Errorf("n_updates = %v, want 1", recorded["n_updates"])
	}
	baseVMAF, _ := recorded["base_vmaf"].(float64)
	residual, _ := recorded["residual"].(float64)
	if got := 91.5 - baseVMAF; got != residual {
		t.Errorf("residual = %v, want observed − base = %v", residual, got)
	}

	statePath, _ := recorded["state_path"].(string)
	if _, err := os.Stat(statePath); err != nil {
		t.Errorf("record should have persisted %s: %v", statePath, err)
	}

	after := runJSON("sidecar", "predict", "--cache-dir", cache,
		"--features-json", features, "--crf", "26", "--json")
	if after["correction"] == 0.0 {
		t.Error("after one capture the correction should be non-zero")
	}
	if after["n_updates"] != 1.0 {
		t.Errorf("the reloaded sidecar reports %v updates, want 1", after["n_updates"])
	}
}

// TestSidecarNoPersist covers the tests-only flag: the fit updates in memory
// but nothing reaches disk.
func TestSidecarNoPersist(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	dir := t.TempDir()
	cache := filepath.Join(dir, "cache")
	features := writeFixture(t, dir, "features.json", featuresJSON)

	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"sidecar", "record", "--cache-dir", cache, "--features-json", features,
			"--crf", "26", "--observed-vmaf", "91.5", "--no-persist", "--json",
		})
		if err := root.Execute(); err != nil {
			t.Fatalf("execute: %v", err)
		}
	})
	var payload map[string]any
	if err := json.Unmarshal([]byte(out), &payload); err != nil {
		t.Fatalf("payload is not JSON: %v", err)
	}
	statePath, _ := payload["state_path"].(string)
	if _, err := os.Stat(statePath); !os.IsNotExist(err) {
		t.Errorf("--no-persist should not have written %s (err=%v)", statePath, err)
	}
}

// TestSidecarBatchRecord pins the skip-and-continue contract: malformed rows
// are reported and skipped, the good rows still land, and the counts are
// reported. An operator's capture log is often partially corrupt after an
// interrupted run; losing the good rows to one bad line would be worse.
func TestSidecarBatchRecord(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	dir := t.TempDir()
	cache := filepath.Join(dir, "cache")

	jsonl := strings.Join([]string{
		`{"probe_bitrate_kbps": 4200.5, "probe_i_frame_avg_bytes": 51200.0,` +
			` "probe_p_frame_avg_bytes": 8100.0, "probe_b_frame_avg_bytes": 2400.0,` +
			` "crf": 24, "observed_vmaf": 94.25}`,
		`{"probe_bitrate_kbps": 1800.0, "probe_i_frame_avg_bytes": 22000.0,` +
			` "probe_p_frame_avg_bytes": 3900.0, "probe_b_frame_avg_bytes": 1100.0,` +
			` "crf": 30, "observed_vmaf": 82.0}`,
		``,
		`{"probe_bitrate_kbps": 900.0, "crf": 35, "observed_vmaf": 71.5}`,
		`{"missing": "everything"}`,
		`not json at all`,
	}, "\n")
	captures := writeFixture(t, dir, "captures.jsonl", jsonl+"\n")

	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"sidecar", "batch-record", "--cache-dir", cache,
			"--captures-jsonl", captures, "--json",
		})
		if err := root.Execute(); err != nil {
			t.Fatalf("execute: %v", err)
		}
	})

	var payload map[string]any
	if err := json.Unmarshal([]byte(out), &payload); err != nil {
		t.Fatalf("payload is not JSON: %v\n%s", err, out)
	}
	if payload["schema"] != "vmaf-tune-sidecar-batch-record/v1" {
		t.Errorf("schema = %v", payload["schema"])
	}
	if payload["rows_recorded"] != 2.0 {
		t.Errorf("rows_recorded = %v, want 2", payload["rows_recorded"])
	}
	if payload["rows_skipped"] != 3.0 {
		t.Errorf("rows_skipped = %v, want 3 (a partial row, a bare object, and a "+
			"non-JSON line)", payload["rows_skipped"])
	}
	if payload["n_updates"] != 2.0 {
		t.Errorf("n_updates = %v, want 2", payload["n_updates"])
	}
	statePath, _ := payload["state_path"].(string)
	if _, err := os.Stat(statePath); err != nil {
		t.Errorf("batch-record should persist once at the end: %v", err)
	}
}

// TestSidecarBatchRecordAllRowsBad checks the no-good-rows path: nothing is
// persisted, but the run still succeeds with the counts reported.
func TestSidecarBatchRecordAllRowsBad(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	dir := t.TempDir()
	cache := filepath.Join(dir, "cache")
	captures := writeFixture(t, dir, "captures.jsonl", "garbage\n{}\n")

	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"sidecar", "batch-record", "--cache-dir", cache,
			"--captures-jsonl", captures, "--json",
		})
		if err := root.Execute(); err != nil {
			t.Fatalf("execute: %v", err)
		}
	})
	var payload map[string]any
	if err := json.Unmarshal([]byte(out), &payload); err != nil {
		t.Fatalf("payload is not JSON: %v", err)
	}
	if payload["rows_recorded"] != 0.0 || payload["rows_skipped"] != 2.0 {
		t.Errorf("counts = (%v recorded, %v skipped), want (0, 2)",
			payload["rows_recorded"], payload["rows_skipped"])
	}
	statePath, _ := payload["state_path"].(string)
	if _, err := os.Stat(statePath); !os.IsNotExist(err) {
		t.Errorf("no good rows means nothing to persist; %s should not exist", statePath)
	}
}

// TestSidecarRejectsBadInput covers the validation paths.
func TestSidecarRejectsBadInput(t *testing.T) {
	dir := t.TempDir()
	features := writeFixture(t, dir, "features.json", featuresJSON)
	notObject := writeFixture(t, dir, "array.json", `[1, 2, 3]`)
	partial := writeFixture(t, dir, "partial.json", `{"probe_bitrate_kbps": 1000}`)
	broken := writeFixture(t, dir, "broken.json", `{nope`)

	tests := []struct {
		name    string
		args    []string
		wantErr string
	}{
		{
			name:    "unknown codec",
			args:    []string{"sidecar", "status", "--codec", "libnope"},
			wantErr: "libnope",
		},
		{
			name: "features file is not an object",
			args: []string{"sidecar", "predict", "--features-json", notObject,
				"--crf", "26"},
			wantErr: "must contain a JSON object",
		},
		{
			name: "features file is missing required keys",
			args: []string{"sidecar", "predict", "--features-json", partial,
				"--crf", "26"},
			wantErr: "missing required keys",
		},
		{
			name: "features file is unparseable",
			args: []string{"sidecar", "predict", "--features-json", broken,
				"--crf", "26"},
			wantErr: "not valid JSON",
		},
		{
			name: "features file does not exist",
			args: []string{"sidecar", "predict", "--features-json",
				filepath.Join(dir, "absent.json"), "--crf", "26"},
			wantErr: "cannot read",
		},
		{
			name: "captures file does not exist",
			args: []string{"sidecar", "batch-record", "--captures-jsonl",
				filepath.Join(dir, "absent.jsonl")},
			wantErr: "cannot read input",
		},
		{
			name:    "record without --observed-vmaf",
			args:    []string{"sidecar", "record", "--features-json", features, "--crf", "26"},
			wantErr: "observed-vmaf",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("XDG_CACHE_HOME", t.TempDir())
			root := newRoot("dev")
			args := append([]string{}, tc.args...)
			args = append(args, "--cache-dir", t.TempDir())
			root.Cobra().SetArgs(args)
			root.Cobra().SetOut(new(strings.Builder))
			root.Cobra().SetErr(new(strings.Builder))

			err := root.Execute()
			if err == nil {
				t.Fatalf("expected an error mentioning %q", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error %q should mention %q", err, tc.wantErr)
			}
		})
	}
}

// TestSidecarFeaturesFromMapping covers the parser directly, including the
// {"features": {...}} wrapper a capture row uses.
func TestSidecarFeaturesFromMapping(t *testing.T) {
	t.Parallel()

	base := map[string]any{
		"probe_bitrate_kbps":      4200.5,
		"probe_i_frame_avg_bytes": 51200.0,
		"probe_p_frame_avg_bytes": 8100.0,
		"probe_b_frame_avg_bytes": 2400.0,
	}

	t.Run("bare object with defaults", func(t *testing.T) {
		t.Parallel()
		got, err := sidecarFeaturesFromMapping(base)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		want := predictor.ShotFeatures{
			ProbeBitrateKbps: 4200.5, ProbeIFrameAvgBytes: 51200.0,
			ProbePFrameAvgBytes: 8100.0, ProbeBFrameAvgBytes: 2400.0,
		}
		if got != want {
			t.Errorf("features = %+v, want %+v", got, want)
		}
	})

	t.Run("features wrapper", func(t *testing.T) {
		t.Parallel()
		wrapped := map[string]any{"features": base, "crf": 24.0, "observed_vmaf": 90.0}
		got, err := sidecarFeaturesFromMapping(wrapped)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if got.ProbeBitrateKbps != 4200.5 {
			t.Errorf("the wrapper was not unwrapped: %+v", got)
		}
	})

	t.Run("wrapper that is not an object", func(t *testing.T) {
		t.Parallel()
		_, err := sidecarFeaturesFromMapping(map[string]any{"features": 42.0})
		if err == nil || !strings.Contains(err.Error(), "must be a JSON object") {
			t.Errorf("err = %v, want a 'features must be a JSON object' error", err)
		}
	})

	t.Run("non-numeric required value", func(t *testing.T) {
		t.Parallel()
		bad := map[string]any{}
		for k, v := range base {
			bad[k] = v
		}
		bad["probe_bitrate_kbps"] = true
		_, err := sidecarFeaturesFromMapping(bad)
		if err == nil || !strings.Contains(err.Error(), "not numeric") {
			t.Errorf("err = %v, want a non-numeric error", err)
		}
	})

	t.Run("missing keys are listed in order", func(t *testing.T) {
		t.Parallel()
		_, err := sidecarFeaturesFromMapping(map[string]any{"probe_bitrate_kbps": 1.0})
		if err == nil {
			t.Fatal("expected an error")
		}
		want := "features missing required keys: probe_i_frame_avg_bytes, " +
			"probe_p_frame_avg_bytes, probe_b_frame_avg_bytes"
		if err.Error() != want {
			t.Errorf("err = %q, want %q", err, want)
		}
	})
}

// captureStdout runs fn with os.Stdout redirected to a pipe and returns what
// was written. The sidecar handlers print through fmt.Print* by design (the
// Python surface writes to stdout, and operators pipe it into jq), so the
// test has to intercept the real file descriptor.
func captureStdout(t *testing.T, fn func()) string {
	t.Helper()

	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatalf("pipe: %v", err)
	}
	original := os.Stdout
	os.Stdout = writer

	done := make(chan string, 1)
	go func() {
		var sb strings.Builder
		buf := make([]byte, 4096)
		for {
			n, readErr := reader.Read(buf)
			if n > 0 {
				sb.Write(buf[:n])
			}
			if readErr != nil {
				break
			}
		}
		done <- sb.String()
	}()

	defer func() {
		os.Stdout = original
	}()
	fn()
	if err := writer.Close(); err != nil {
		t.Fatalf("close pipe writer: %v", err)
	}
	out := <-done
	if err := reader.Close(); err != nil {
		t.Fatalf("close pipe reader: %v", err)
	}
	return out
}
