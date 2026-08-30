// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/corpus_test.go — in-package tests for the corpus
// subcommand's flag surface and validation.
//
// The flag names and defaults mirror the Python `vmaf-tune corpus` argument
// parser: scripts that drive one binary must drive the other unchanged.

package cmd

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/corpus"
)

func TestCorpusFlagSurface(t *testing.T) {
	t.Parallel()

	cmd := newCorpusCmd()
	tests := []struct {
		flag        string
		wantDefault string
	}{
		{flag: "source", wantDefault: "[]"},
		{flag: "width", wantDefault: "0"},
		{flag: "height", wantDefault: "0"},
		{flag: "pix-fmt", wantDefault: "yuv420p"},
		{flag: "framerate", wantDefault: "24"},
		{flag: "duration", wantDefault: "0"},
		{flag: "encoder", wantDefault: "libx264"},
		{flag: "preset", wantDefault: "[]"},
		{flag: "crf", wantDefault: "[]"},
		{flag: "output", wantDefault: "corpus.jsonl"},
		{flag: "encode-dir", wantDefault: filepath.Join(".workingdir2", "encodes")},
		{flag: "keep-encodes", wantDefault: "false"},
		{flag: "vmaf-model", wantDefault: corpus.Model1080P},
		{flag: "neg", wantDefault: "false"},
		{flag: "ffmpeg-bin", wantDefault: "ffmpeg"},
		{flag: "vmaf-bin", wantDefault: "vmaf"},
		{flag: "ffprobe-bin", wantDefault: "ffprobe"},
		{flag: "score-backend", wantDefault: "auto"},
		{flag: "no-source-hash", wantDefault: "false"},
		{flag: "two-pass", wantDefault: "false"},
		{flag: "sample-clip-seconds", wantDefault: "0"},
		{flag: "coarse-to-fine", wantDefault: "false"},
		{flag: "coarse-step", wantDefault: "10"},
		{flag: "fine-radius", wantDefault: "5"},
		{flag: "fine-step", wantDefault: "1"},
		{flag: "target-vmaf", wantDefault: "0"},
		{flag: "auto-hdr", wantDefault: "false"},
		{flag: "force-sdr", wantDefault: "false"},
		{flag: "force-hdr-pq", wantDefault: "false"},
		{flag: "force-hdr-hlg", wantDefault: "false"},
	}

	for _, tc := range tests {
		t.Run(tc.flag, func(t *testing.T) {
			t.Parallel()
			flag := cmd.Flags().Lookup(tc.flag)
			if flag == nil {
				t.Fatalf("corpus is missing the --%s flag", tc.flag)
			}
			if flag.DefValue != tc.wantDefault {
				t.Errorf("--%s default = %q, want %q", tc.flag, flag.DefValue, tc.wantDefault)
			}
			if flag.Usage == "" {
				t.Errorf("--%s has no usage text", tc.flag)
			}
		})
	}
}

func TestCorpusRequiredFlags(t *testing.T) {
	t.Parallel()

	cmd := newCorpusCmd()
	for _, name := range []string{"source", "width", "height", "preset"} {
		flag := cmd.Flags().Lookup(name)
		if flag == nil {
			t.Fatalf("corpus is missing the --%s flag", name)
		}
		if flag.Annotations["cobra_annotation_bash_completion_one_required_flag"] == nil {
			t.Errorf("--%s should be required", name)
		}
	}
}

func TestValidateCorpusFlags(t *testing.T) {
	t.Parallel()

	valid := func() *corpusFlags {
		return &corpusFlags{
			sources: []string{"clip.yuv"},
			width:   1920, height: 1080,
			presets: []string{"medium"},
			crfs:    []int{26},
			encoder: "libx264",
		}
	}

	tests := []struct {
		name    string
		mutate  func(*corpusFlags)
		wantErr string
	}{
		{name: "a complete full-grid invocation"},
		{
			name:    "no source",
			mutate:  func(f *corpusFlags) { f.sources = nil },
			wantErr: "--source is required",
		},
		{
			name:    "no preset",
			mutate:  func(f *corpusFlags) { f.presets = nil },
			wantErr: "--preset is required",
		},
		{
			name:    "no crf and no coarse-to-fine",
			mutate:  func(f *corpusFlags) { f.crfs = nil },
			wantErr: "--crf is required",
		},
		{
			name: "coarse-to-fine derives the crf axis itself",
			mutate: func(f *corpusFlags) {
				f.crfs = nil
				f.coarseToFine = true
			},
		},
		{
			name:    "a zero width",
			mutate:  func(f *corpusFlags) { f.width = 0 },
			wantErr: "--width and --height must be positive",
		},
		{
			name:    "a negative height",
			mutate:  func(f *corpusFlags) { f.height = -1 },
			wantErr: "--width and --height must be positive",
		},
		{
			name:    "an unknown encoder",
			mutate:  func(f *corpusFlags) { f.encoder = "libx999" },
			wantErr: "unknown codec",
		},
		{
			name:    "an out-of-range target",
			mutate:  func(f *corpusFlags) { f.targetVMAF = 101 },
			wantErr: "out of range",
		},
		{
			name:   "an unset target is not treated as out of range",
			mutate: func(f *corpusFlags) { f.targetVMAF = 0 },
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			flags := valid()
			if tc.mutate != nil {
				tc.mutate(flags)
			}
			err := validateCorpusFlags(flags)
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("validateCorpusFlags: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatalf("validateCorpusFlags accepted %+v", flags)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error = %q, want it to contain %q", err, tc.wantErr)
			}
		})
	}
}

func TestResolveHDRMode(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		mutate func(*corpusFlags)
		want   string
	}{
		{name: "auto is the default", want: corpus.HDRModeAuto},
		{
			name:   "--auto-hdr is explicit auto",
			mutate: func(f *corpusFlags) { f.autoHDR = true },
			want:   corpus.HDRModeAuto,
		},
		{
			name:   "--force-sdr",
			mutate: func(f *corpusFlags) { f.forceSDR = true },
			want:   corpus.HDRModeForceSDR,
		},
		{
			name:   "--force-hdr-pq",
			mutate: func(f *corpusFlags) { f.forceHDRPQ = true },
			want:   corpus.HDRModeForcePQ,
		},
		{
			name:   "--force-hdr-hlg",
			mutate: func(f *corpusFlags) { f.forceHDRHLG = true },
			want:   corpus.HDRModeForceHLG,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			flags := &corpusFlags{}
			if tc.mutate != nil {
				tc.mutate(flags)
			}
			if got := flags.resolveHDRMode(); got != tc.want {
				t.Errorf("resolveHDRMode() = %q, want %q", got, tc.want)
			}
		})
	}
}

// TestResolveVMAFModel_corpusCases is the corpus group's independent table
// for resolveVMAFModel (defined in pershot.go). The per-shot group wrote its
// own table in pershot_internal_test.go; both are kept because they cover
// different inputs, so the name here is qualified to avoid a redeclaration.
func TestResolveVMAFModel_corpusCases(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		model string
		neg   bool
		want  string
	}{
		{name: "the default model", model: corpus.Model1080P, want: corpus.Model1080P},
		{name: "--neg routes to the NEG variant", model: corpus.Model1080P, neg: true,
			want: corpus.Model1080PNEG},
		{name: "--neg on the 4K model", model: corpus.Model4K, neg: true,
			want: corpus.Model4KNEG},
		{name: "--neg is idempotent", model: corpus.Model1080PNEG, neg: true,
			want: corpus.Model1080PNEG},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			flags := &corpusFlags{vmafModel: tc.model, neg: tc.neg}
			if got := flags.resolveVMAFModel(); got != tc.want {
				t.Errorf("resolveVMAFModel() = %q, want %q", got, tc.want)
			}
		})
	}
}

func TestBuildCorpusJob(t *testing.T) {
	t.Parallel()

	flags := &corpusFlags{
		width: 1920, height: 1080, pixFmt: "yuv420p10le",
		framerate: 23.976, duration: 10,
	}
	cells := []corpus.Cell{{Preset: "medium", CRF: 26}}
	got := buildCorpusJob(flags, "/refs/clip.yuv", cells)

	if got.Source != "/refs/clip.yuv" {
		t.Errorf("Source = %q, want /refs/clip.yuv", got.Source)
	}
	if got.Width != 1920 || got.Height != 1080 {
		t.Errorf("geometry = %dx%d, want 1920x1080", got.Width, got.Height)
	}
	if got.PixFmt != "yuv420p10le" {
		t.Errorf("PixFmt = %q, want yuv420p10le", got.PixFmt)
	}
	if got.Framerate != 23.976 || got.DurationS != 10 {
		t.Errorf("timing = (%v, %v), want (23.976, 10)", got.Framerate, got.DurationS)
	}
	if len(got.Cells) != 1 || got.Cells[0] != cells[0] {
		t.Errorf("Cells = %v, want %v", got.Cells, cells)
	}
}

func TestCorpusWriter(t *testing.T) {
	t.Parallel()

	t.Run("rows stream to disk as they complete", func(t *testing.T) {
		t.Parallel()
		path := filepath.Join(t.TempDir(), "nested", "corpus.jsonl")
		w, err := newCorpusWriter(path)
		if err != nil {
			t.Fatalf("newCorpusWriter: %v", err)
		}
		row := map[string]any{}
		for _, key := range corpus.RowKeys {
			row[key] = 0
		}
		row["crf"] = 26
		if emitErr := w.emit(row); emitErr != nil {
			t.Fatalf("emit: %v", emitErr)
		}
		if closeErr := w.close(); closeErr != nil {
			t.Fatalf("close: %v", closeErr)
		}
		if w.rows != 1 {
			t.Errorf("rows = %d, want 1", w.rows)
		}
		data, readErr := os.ReadFile(path) //nolint:gosec // test-owned temp path
		if readErr != nil {
			t.Fatalf("read output: %v", readErr)
		}
		if !strings.Contains(string(data), `"crf": 26`) {
			t.Errorf("output does not carry the row: %s", data)
		}
	})

	t.Run("an incomplete row is refused", func(t *testing.T) {
		t.Parallel()
		path := filepath.Join(t.TempDir(), "corpus.jsonl")
		w, err := newCorpusWriter(path)
		if err != nil {
			t.Fatalf("newCorpusWriter: %v", err)
		}
		defer func() { _ = w.close() }()

		// A row missing schema keys would poison the Phase B/C contract.
		if emitErr := w.emit(map[string]any{"crf": 26}); emitErr == nil {
			t.Fatal("emit accepted a row missing schema keys")
		} else if !strings.Contains(emitErr.Error(), "missing keys") {
			t.Errorf("error = %q, want it to name the missing keys", emitErr)
		}
	})

	t.Run("an unwritable path errors", func(t *testing.T) {
		t.Parallel()
		// A path whose parent is an existing regular file cannot be created.
		dir := t.TempDir()
		blocker := filepath.Join(dir, "blocker")
		if err := os.WriteFile(blocker, []byte("x"), 0o600); err != nil {
			t.Fatalf("seed blocker: %v", err)
		}
		if _, err := newCorpusWriter(filepath.Join(blocker, "corpus.jsonl")); err == nil {
			t.Error("newCorpusWriter accepted an uncreatable path")
		}
	})
}

func TestCorpusCommandHelpNamesBothSearchModes(t *testing.T) {
	t.Parallel()

	long := newCorpusCmd().Long
	for _, needle := range []string{"full grid", "coarse-to-fine", "--target-vmaf"} {
		if !strings.Contains(long, needle) {
			t.Errorf("the corpus long help does not mention %q", needle)
		}
	}
}
