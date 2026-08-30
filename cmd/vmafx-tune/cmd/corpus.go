// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/corpus"
)

// corpusFlags holds the flags parsed by the corpus subcommand. Names and
// defaults mirror the Python `vmaf-tune corpus` argument parser exactly.
type corpusFlags struct {
	sources   []string
	width     int
	height    int
	pixFmt    string
	framerate float64
	duration  float64

	encoder string
	presets []string
	crfs    []int

	output      string
	encodeDir   string
	keepEncodes bool

	vmafModel string
	neg       bool

	ffmpegBin  string
	vmafBin    string
	ffprobeBin string

	scoreBackend      string
	noSourceHash      bool
	twoPass           bool
	sampleClipSeconds float64

	// Coarse-to-fine (_add_coarse_to_fine_flags).
	coarseToFine bool
	coarseStep   int
	fineRadius   int
	fineStep     int
	targetVMAF   float64

	// HDR mode — the mutually-exclusive --auto-hdr / --force-* group.
	autoHDR     bool
	forceSDR    bool
	forceHDRPQ  bool
	forceHDRHLG bool
}

// newCorpusCmd builds and returns the "corpus" cobra subcommand.
func newCorpusCmd() *cobra.Command {
	flags := &corpusFlags{}

	cmd := clikit.Command("corpus",
		"Run the Phase A (preset, crf) grid sweep and emit a JSONL corpus",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runCorpus(ctx, d, flags)
		})),
	)
	cmd.Long = `Sweep a (preset, crf) grid against one or more references, encode each cell,
score it against the reference with the libvmaf CLI, and write one JSONL row per
(source, preset, crf) combination.

The JSONL schema (v3) is the API contract the Phase B target-VMAF bisect and the
Phase C per-title CRF predictor consume. Each row carries the encode provenance
(source hash, encoder / ffmpeg versions, argv extras), the measured bitrate and
VMAF, the canonical-6 libvmaf feature aggregates, the HDR provenance triple, the
TransNet-V2 shot-metadata trio, and ten encoder-internal stats aggregates.

Two search modes:

  full grid        --preset P --crf C (both repeatable) sweeps every cell.
  coarse-to-fine   --coarse-to-fine runs a 2-pass search instead: a coarse
                   sweep at --coarse-step, then a fine sweep within
                   +/- --fine-radius of the best coarse point. With the
                   defaults that is 15 encodes instead of 52 (ADR-0296).

Example — full grid:
  vmafx-tune-go corpus \
    --source ref.yuv \
    --width 1920 --height 1080 \
    --framerate 24 --duration 10 \
    --preset medium --crf 20 --crf 26 --crf 32 \
    --output corpus.jsonl

Example — coarse-to-fine against a VMAF target:
  vmafx-tune-go corpus \
    --source ref.yuv --width 1920 --height 1080 \
    --preset medium --coarse-to-fine --target-vmaf 93 \
    --output corpus.jsonl`

	cmd.Flags().StringArrayVar(&flags.sources, "source", nil,
		"Reference video (repeat for multiple sources; required)")
	cmd.Flags().IntVar(&flags.width, "width", 0, "Rung target width in pixels (required)")
	cmd.Flags().IntVar(&flags.height, "height", 0, "Rung target height in pixels (required)")
	cmd.Flags().StringVar(&flags.pixFmt, "pix-fmt", "yuv420p",
		"ffmpeg pix_fmt (default yuv420p)")
	cmd.Flags().Float64Var(&flags.framerate, "framerate", 24.0, "Reference framerate")
	cmd.Flags().Float64Var(&flags.duration, "duration", 0.0,
		"Reference duration in seconds (bounds the encode and the bitrate calc)")
	cmd.Flags().StringVar(&flags.encoder, "encoder", "libx264",
		"Codec adapter; one of "+strings.Join(codecadapter.Known(), ", "))
	cmd.Flags().StringArrayVar(&flags.presets, "preset", nil,
		"Encoder preset (repeatable; required)")
	cmd.Flags().IntSliceVar(&flags.crfs, "crf", nil,
		"Quality value (repeatable). Required unless --coarse-to-fine picks the axis")
	cmd.Flags().StringVar(&flags.output, "output", "corpus.jsonl",
		"JSONL output path (default corpus.jsonl)")
	cmd.Flags().StringVar(&flags.encodeDir, "encode-dir",
		filepath.Join(".workingdir2", "encodes"),
		"Scratch dir for encodes (default .workingdir2/encodes, gitignored)")
	cmd.Flags().BoolVar(&flags.keepEncodes, "keep-encodes", false,
		"Retain encoded outputs after scoring (default: delete)")
	cmd.Flags().StringVar(&flags.vmafModel, "vmaf-model", corpus.Model1080P,
		"vmaf model version string (default "+corpus.Model1080P+")")
	cmd.Flags().BoolVar(&flags.neg, "neg", false,
		"Use the VMAF NEG (No Enhancement Gain) model variant, which penalises "+
			"sharpening-based score inflation. Use for codec A-vs-B comparisons; do NOT "+
			"use for production quality monitoring against baselines. See "+
			"docs/metrics/vmaf-neg.md (ADR-0622)")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg", "Path to the ffmpeg binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf-bin", "vmaf", "Path to the vmaf binary")
	cmd.Flags().StringVar(&flags.ffprobeBin, "ffprobe-bin", "ffprobe",
		"Path to the ffprobe binary (default: ffprobe on PATH)")
	cmd.Flags().StringVar(&flags.scoreBackend, "score-backend", "auto",
		"libvmaf scoring backend: auto, "+strings.Join(corpus.AllBackends, ", ")+
			". 'auto' picks the fastest available (cuda > sycl > hip > cpu); a specific "+
			"name is honoured strictly and errors out if unavailable")
	cmd.Flags().BoolVar(&flags.noSourceHash, "no-source-hash", false,
		"Skip src_sha256 (faster on huge YUVs; loses provenance)")
	cmd.Flags().BoolVar(&flags.twoPass, "two-pass", false,
		"Run a 2-pass encode for codecs that support it (libx264 / libx265 today). "+
			"Adapters where two-pass is unsupported fall back to single-pass with a "+
			"stderr warning (ADR-0333)")
	cmd.Flags().Float64Var(&flags.sampleClipSeconds, "sample-clip-seconds", 0.0,
		"Encode/score only the centre N-second slice of each source (default 0 = full "+
			"source). Encode time scales linearly with the slice length; expect a 1-2 "+
			"VMAF-point delta vs full-clip on diverse content (ADR-0297)")

	cmd.Flags().BoolVar(&flags.coarseToFine, "coarse-to-fine", false,
		"Run a 2-pass coarse-then-fine CRF search instead of the full grid (ADR-0296)")
	cmd.Flags().IntVar(&flags.coarseStep, "coarse-step", 10,
		"CRF step for the coarse pass (default 10 -> [10,20,30,40,50])")
	cmd.Flags().IntVar(&flags.fineRadius, "fine-radius", 5,
		"+/- radius around the best-coarse CRF for the fine pass (default 5)")
	cmd.Flags().IntVar(&flags.fineStep, "fine-step", 1, "CRF step for the fine pass (default 1)")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", 0,
		"Target VMAF score; the orchestrator refines around the smallest CRF whose "+
			"score >= target. Optional for corpus")

	cmd.Flags().BoolVar(&flags.autoHDR, "auto-hdr", false,
		"(default) probe each source via ffprobe and inject HDR codec args + the "+
			"HDR-VMAF model when PQ / HLG signalling is detected")
	cmd.Flags().BoolVar(&flags.forceSDR, "force-sdr", false,
		"Treat all sources as SDR; skip HDR detection and flag injection")
	cmd.Flags().BoolVar(&flags.forceHDRPQ, "force-hdr-pq", false,
		"Treat all sources as HDR PQ (SMPTE-2084) regardless of probe")
	cmd.Flags().BoolVar(&flags.forceHDRHLG, "force-hdr-hlg", false,
		"Treat all sources as HDR HLG (ARIB STD-B67) regardless of probe")
	cmd.MarkFlagsMutuallyExclusive("auto-hdr", "force-sdr", "force-hdr-pq", "force-hdr-hlg")

	_ = cmd.MarkFlagRequired("source")
	_ = cmd.MarkFlagRequired("width")
	_ = cmd.MarkFlagRequired("height")
	_ = cmd.MarkFlagRequired("preset")

	return cmd
}

// resolveHDRMode maps the mutually-exclusive HDR flag group onto the
// corpus.HDRMode* constant, defaulting to "auto".
func (f *corpusFlags) resolveHDRMode() string {
	switch {
	case f.forceSDR:
		return corpus.HDRModeForceSDR
	case f.forceHDRPQ:
		return corpus.HDRModeForcePQ
	case f.forceHDRHLG:
		return corpus.HDRModeForceHLG
	default:
		return corpus.HDRModeAuto
	}
}

// resolveVMAFModel applies the --neg routing to the configured model version.
func (f *corpusFlags) resolveVMAFModel() string {
	if f.neg {
		return corpus.NegModelFor(f.vmafModel)
	}
	return f.vmafModel
}

// buildCorpusOptions assembles the corpus Options from the parsed flags,
// resolving the scoring backend up front so an unavailable backend errors out
// before any encode cycles are burned (ADR-0299 / ADR-0314).
func buildCorpusOptions(ctx context.Context, flags *corpusFlags) (corpus.Options, error) {
	selected, err := corpus.SelectBackend(ctx, flags.scoreBackend, nil, nil, flags.vmafBin, nil)
	if err != nil {
		return corpus.Options{}, err
	}
	fmt.Fprintf(os.Stderr, "vmafx-tune: scoring backend = %s\n", selected)

	opts := corpus.NewOptions()
	opts.Encoder = flags.encoder
	opts.Output = flags.output
	opts.EncodeDir = flags.encodeDir
	opts.VMAFModel = flags.resolveVMAFModel()
	opts.FFmpegBin = flags.ffmpegBin
	opts.VMAFBin = flags.vmafBin
	opts.FFprobeBin = flags.ffprobeBin
	opts.KeepEncodes = flags.keepEncodes
	opts.SrcSHA256 = !flags.noSourceHash
	opts.SampleClipSeconds = flags.sampleClipSeconds
	opts.ScoreBackend = selected
	opts.HDRMode = flags.resolveHDRMode()
	opts.TwoPass = flags.twoPass
	return opts, nil
}

// buildCorpusJob assembles the per-source Job.
func buildCorpusJob(flags *corpusFlags, src string, cells []corpus.Cell) corpus.Job {
	return corpus.Job{
		Source:    src,
		Width:     flags.width,
		Height:    flags.height,
		PixFmt:    flags.pixFmt,
		Framerate: flags.framerate,
		DurationS: flags.duration,
		Cells:     cells,
	}
}

// validateCorpusFlags rejects flag combinations the sweep cannot run.
func validateCorpusFlags(flags *corpusFlags) error {
	if len(flags.sources) == 0 {
		return errors.New("--source is required")
	}
	if flags.width <= 0 || flags.height <= 0 {
		return fmt.Errorf("--width and --height must be positive (got %dx%d)",
			flags.width, flags.height)
	}
	if len(flags.presets) == 0 {
		return errors.New("--preset is required")
	}
	if _, err := codecadapter.Get(flags.encoder); err != nil {
		return err
	}
	if !flags.coarseToFine && len(flags.crfs) == 0 {
		return errors.New("--crf is required (or use --coarse-to-fine)")
	}
	if flags.targetVMAF != 0 && (flags.targetVMAF <= 0 || flags.targetVMAF > 100) {
		return fmt.Errorf("--target-vmaf %g is out of range (0, 100]", flags.targetVMAF)
	}
	return nil
}

// runCorpus is the implementation of the corpus subcommand.
func runCorpus(ctx context.Context, d deps, flags *corpusFlags) error {
	if err := validateCorpusFlags(flags); err != nil {
		return err
	}
	opts, err := buildCorpusOptions(ctx, flags)
	if err != nil {
		return err
	}

	writer, err := newCorpusWriter(opts.Output)
	if err != nil {
		return err
	}

	d.Log.InfoContext(ctx, "starting corpus sweep",
		"sources", flags.sources,
		"encoder", opts.Encoder,
		"presets", flags.presets,
		"coarse_to_fine", flags.coarseToFine,
		"output", opts.Output,
		"score_backend", opts.ScoreBackend)

	sweepErr := sweepCorpus(ctx, flags, opts, writer.emit)
	closeErr := writer.close()
	if sweepErr != nil {
		return sweepErr
	}
	if closeErr != nil {
		return closeErr
	}

	if flags.coarseToFine {
		fmt.Fprintf(os.Stderr, "coarse-to-fine: wrote %d rows -> %s\n", writer.rows, opts.Output)
	} else {
		fmt.Fprintf(os.Stderr, "wrote %d rows -> %s\n", writer.rows, opts.Output)
	}
	d.Log.InfoContext(ctx, "corpus sweep complete", "rows", writer.rows, "output", opts.Output)
	return nil
}

// sweepCorpus drives every source through the selected search mode.
func sweepCorpus(
	ctx context.Context, flags *corpusFlags, opts corpus.Options,
	emit func(map[string]any) error,
) error {
	if flags.coarseToFine {
		params := corpus.DefaultCoarseToFineParams()
		params.CoarseStep = flags.coarseStep
		params.FineRadius = flags.fineRadius
		params.FineStep = flags.fineStep
		if flags.targetVMAF != 0 {
			target := flags.targetVMAF
			params.TargetVMAF = &target
		}
		// Coarse-to-fine ignores --crf and derives the CRF axis itself; the
		// sentinel cells only carry the preset axis.
		sentinel := make([]corpus.Cell, len(flags.presets))
		for i, p := range flags.presets {
			sentinel[i] = corpus.Cell{Preset: p, CRF: 0}
		}
		for _, src := range flags.sources {
			job := buildCorpusJob(flags, src, sentinel)
			if err := corpus.CoarseToFineSearch(ctx, job, opts, corpus.Runners{},
				params, emit); err != nil {
				return err
			}
		}
		return nil
	}

	cells := corpus.IterGrid(flags.presets, flags.crfs)
	for _, src := range flags.sources {
		job := buildCorpusJob(flags, src, cells)
		if err := corpus.IterRows(ctx, job, opts, corpus.Runners{}, emit); err != nil {
			return err
		}
	}
	return nil
}

// corpusWriter streams rows to the JSONL output as they complete, so a long
// sweep leaves a usable partial corpus if it is interrupted.
type corpusWriter struct {
	file *os.File
	rows int
}

// newCorpusWriter opens (creating / truncating) the JSONL output.
func newCorpusWriter(path string) (*corpusWriter, error) {
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o750); err != nil {
			return nil, fmt.Errorf("create corpus output dir: %w", err)
		}
	}
	// 0o600: corpus rows carry source paths that can leak dataset
	// identifiers; restrict to the owner.
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600) // #nosec G304 -- operator-supplied --output path.
	if err != nil {
		return nil, fmt.Errorf("open corpus output: %w", err)
	}
	return &corpusWriter{file: f}, nil
}

// emit appends one row to the JSONL output.
func (w *corpusWriter) emit(row map[string]any) error {
	if missing := corpus.MissingRowKeys(row); len(missing) > 0 {
		return fmt.Errorf("corpus row missing keys: %v", missing)
	}
	line, err := corpus.WriteRowLine(row)
	if err != nil {
		return fmt.Errorf("encode corpus row %d: %w", w.rows, err)
	}
	if _, err := w.file.WriteString(line + "\n"); err != nil {
		return fmt.Errorf("write corpus row %d: %w", w.rows, err)
	}
	w.rows++
	return nil
}

// close syncs and closes the output file.
func (w *corpusWriter) close() error {
	syncErr := w.file.Sync()
	closeErr := w.file.Close()
	return errors.Join(syncErr, closeErr)
}
