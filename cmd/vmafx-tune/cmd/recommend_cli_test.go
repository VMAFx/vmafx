// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// CLI-level tests for the `recommend` subcommand's --from-corpus path, which
// runs entirely on data and needs no ffmpeg or vmaf binary.

package cmd

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// writeCorpus writes a JSONL corpus fixture and returns its path.
func writeCorpus(t *testing.T, lines ...string) string {
	t.Helper()

	path := filepath.Join(t.TempDir(), "corpus.jsonl")
	body := strings.Join(lines, "\n") + "\n"
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("write corpus: %v", err)
	}
	return path
}

// corpusFixture is a three-row corpus spanning the target.
var corpusFixture = []string{
	`{"encoder":"libx264","preset":"medium","src":"/a.yuv","crf":20,` +
		`"vmaf_score":96.0,"bitrate_kbps":8000,"exit_status":0}`,
	`{"encoder":"libx264","preset":"medium","src":"/a.yuv","crf":24,` +
		`"vmaf_score":93.5,"bitrate_kbps":5000,"exit_status":0}`,
	`{"encoder":"libx264","preset":"medium","src":"/a.yuv","crf":28,` +
		`"vmaf_score":90.0,"bitrate_kbps":3000,"exit_status":0}`,
}

// TestRecommend_fromCorpusTargetVMAF drives the whole clikit root and asserts
// the JSON output is the smallest-CRF passing row.
func TestRecommend_fromCorpusTargetVMAF(t *testing.T) {
	corpus := writeCorpus(t, corpusFixture...)

	var execErr error
	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"recommend", "--from-corpus", corpus,
			"--target-vmaf", "93", "--json",
		})
		execErr = root.Execute()
	})
	if execErr != nil {
		t.Fatalf("execute recommend: %v", execErr)
	}

	var row map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(out)), &row); err != nil {
		t.Fatalf("parse recommendation JSON %q: %v", out, err)
	}
	if crf, ok := row["crf"].(float64); !ok || int(crf) != 20 {
		t.Errorf("recommended crf = %v, want 20", row["crf"])
	}
}

// TestRecommend_fromCorpusTargetBitrate covers the second predicate.
func TestRecommend_fromCorpusTargetBitrate(t *testing.T) {
	corpus := writeCorpus(t, corpusFixture...)

	var execErr error
	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"recommend", "--from-corpus", corpus,
			"--target-bitrate", "5100", "--json",
		})
		execErr = root.Execute()
	})
	if execErr != nil {
		t.Fatalf("execute recommend: %v", execErr)
	}

	var row map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(out)), &row); err != nil {
		t.Fatalf("parse recommendation JSON %q: %v", out, err)
	}
	if crf, ok := row["crf"].(float64); !ok || int(crf) != 24 {
		t.Errorf("recommended crf = %v, want 24 (closest to 5100 kbps)", row["crf"])
	}
}

// TestRecommend_fromCorpusHumanReadable pins the non-JSON output line.
func TestRecommend_fromCorpusHumanReadable(t *testing.T) {
	corpus := writeCorpus(t, corpusFixture...)

	var execErr error
	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"recommend", "--from-corpus", corpus, "--target-vmaf", "93",
		})
		execErr = root.Execute()
	})
	if execErr != nil {
		t.Fatalf("execute recommend: %v", execErr)
	}
	for _, want := range []string{"crf=20", "vmaf=96.000", "kbps=8000",
		"predicate=target_vmaf>=93.0", "[OK]"} {
		if !strings.Contains(out, want) {
			t.Errorf("output %q is missing %q", strings.TrimSpace(out), want)
		}
	}
}

// TestRecommend_fromCorpusUnmet asserts the UNMET tag when nothing clears the
// bar, and that the closest miss is still reported rather than an error.
func TestRecommend_fromCorpusUnmet(t *testing.T) {
	corpus := writeCorpus(t, corpusFixture...)

	var execErr error
	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"recommend", "--from-corpus", corpus, "--target-vmaf", "99",
		})
		execErr = root.Execute()
	})
	if execErr != nil {
		t.Fatalf("execute recommend: %v", execErr)
	}
	if !strings.Contains(out, "[UNMET]") || !strings.Contains(out, "crf=20") {
		t.Errorf("output %q should report the closest miss tagged UNMET",
			strings.TrimSpace(out))
	}
}

// TestRecommend_fromCorpusWithUncertainty covers the interval-aware path,
// including the short-circuit visited count.
func TestRecommend_fromCorpusWithUncertainty(t *testing.T) {
	corpus := writeCorpus(t,
		`{"encoder":"libx264","preset":"medium","crf":20,"vmaf_score":96.0,`+
			`"bitrate_kbps":8000,"exit_status":0,`+
			`"vmaf_interval":{"low":95.0,"high":96.5}}`,
		`{"encoder":"libx264","preset":"medium","crf":24,"vmaf_score":93.5,`+
			`"bitrate_kbps":5000,"exit_status":0,`+
			`"vmaf_interval":{"low":93.0,"high":94.0}}`,
	)

	var execErr error
	out := captureStdout(t, func() {
		root := newRoot("dev")
		root.Cobra().SetArgs([]string{
			"recommend", "--from-corpus", corpus,
			"--target-vmaf", "93", "--with-uncertainty",
		})
		execErr = root.Execute()
	})
	if execErr != nil {
		t.Fatalf("execute recommend: %v", execErr)
	}
	for _, want := range []string{"decision=tight", "visited=1/2", "TIGHT"} {
		if !strings.Contains(out, want) {
			t.Errorf("output %q is missing %q", strings.TrimSpace(out), want)
		}
	}
}

// TestRecommend_errors covers the flag-validation rejections.
func TestRecommend_errors(t *testing.T) {
	corpus := writeCorpus(t, corpusFixture...)

	tests := []struct {
		name string
		args []string
	}{
		{
			name: "both targets",
			args: []string{
				"recommend", "--from-corpus", corpus,
				"--target-vmaf", "93", "--target-bitrate", "5000",
			},
		},
		{
			name: "no target",
			args: []string{"recommend", "--from-corpus", corpus},
		},
		{
			name: "missing corpus file",
			args: []string{
				"recommend", "--from-corpus", "/nonexistent/corpus.jsonl",
				"--target-vmaf", "93",
			},
		},
		{
			name: "encode path without a source",
			args: []string{"recommend", "--target-vmaf", "93"},
		},
		{
			name: "encode path without a target",
			args: []string{
				"recommend", "--source", "/a.yuv", "--width", "1920",
				"--height", "1080", "--preset", "medium",
			},
		},
		{
			name: "unknown encoder",
			args: []string{
				"recommend", "--source", "/a.yuv", "--width", "1920",
				"--height", "1080", "--preset", "medium",
				"--target-vmaf", "93", "--encoder", "libtheora",
			},
		},
		{
			name: "unknown preset for the encoder",
			args: []string{
				"recommend", "--source", "/a.yuv", "--width", "1920",
				"--height", "1080", "--preset", "turbo", "--target-vmaf", "93",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			root := newRoot("dev")
			root.Cobra().SetArgs(tc.args)
			root.Cobra().SetOut(&strings.Builder{})
			root.Cobra().SetErr(&strings.Builder{})
			if err := root.Execute(); err == nil {
				t.Error("expected an error")
			}
		})
	}
}

// TestRecommend_flagSurface asserts every Python flag name is present, since
// operators and scripts move between the two binaries.
func TestRecommend_flagSurface(t *testing.T) {
	t.Parallel()

	cmd := newRecommendCmd()
	want := []string{
		"source", "width", "height", "pix-fmt", "framerate", "duration",
		"encoder", "preset", "output", "encode-dir", "keep-encodes",
		"vmaf-model", "ffmpeg-bin", "vmaf-bin", "score-backend",
		"no-source-hash", "coarse-to-fine", "coarse-step", "fine-radius",
		"fine-step", "target-vmaf", "with-uncertainty", "uncertainty-sidecar",
		"from-corpus", "target-bitrate", "json",
	}
	for _, name := range want {
		if cmd.Flags().Lookup(name) == nil {
			t.Errorf("recommend is missing the --%s flag", name)
		}
	}
}
