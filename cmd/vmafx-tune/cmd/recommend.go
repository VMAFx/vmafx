// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/internal/pyjson"
	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/corpusrow"
	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/recommend"
	"github.com/VMAFx/vmafx/pkg/scorecli"
	"github.com/VMAFx/vmafx/pkg/uncertainty"
)

// recommendFlags mirrors the Python `vmaf-tune recommend` flag surface
// (_add_recommend_args + _add_coarse_to_fine_flags +
// _add_recommend_uncertainty_flags).
type recommendFlags struct {
	sources   []string
	width     int
	height    int
	pixFmt    string
	framerate float64
	duration  float64
	encoder   string
	presets   []string
	output    string
	encodeDir string

	keepEncodes  bool
	vmafModel    string
	ffmpegBin    string
	vmafBin      string
	scoreBackend string
	noSourceHash bool

	coarseToFine bool
	coarseStep   int
	fineRadius   int
	fineStep     int
	targetVMAF   float64

	withUncertainty    bool
	uncertaintySidecar string

	fromCorpus    string
	targetBitrate float64
	jsonOutput    bool
}

// newRecommendCmd builds the "recommend" cobra subcommand.
func newRecommendCmd() *cobra.Command {
	flags := &recommendFlags{}

	cmd := clikit.Command("recommend",
		"Find the smallest CRF whose VMAF meets --target-vmaf",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runRecommend(ctx, d, flags)
		})),
	)
	cmd.Long = `Pick the CRF that meets a quality or bitrate target.

Two modes:

  --from-corpus JSONL   Pick from an existing corpus without running any new
                        encodes. Applies one of two predicates:
                          --target-vmaf T     smallest CRF whose VMAF >= T
                                              (smaller CRF = higher quality,
                                              so this is the best quality that
                                              clears the gate)
                          --target-bitrate B  row whose bitrate is closest to
                                              B kbps, ties to the lower CRF
                        The two targets are mutually exclusive.

  (default)             Run a coarse-to-fine CRF search over --source, writing
                        every visited encode to --output as corpus JSONL, then
                        pick from those rows. With the defaults that is 5
                        coarse plus up to 10 fine encodes rather than 52 for a
                        full sweep (ADR-0296).

--with-uncertainty consumes the conformal prediction intervals carried in the
rows' vmaf_interval blocks (ADR-0279). A tight interval whose lower bound
already clears the target short-circuits the search; a wide interval refuses
to short-circuit and tags the result (UNCERTAIN). It changes which encodes get
probed, never which get shipped.

Examples:
  vmafx-tune-go recommend --from-corpus corpus.jsonl --target-vmaf 93 --json

  vmafx-tune-go recommend \
    --source src.yuv --width 1920 --height 1080 --preset medium \
    --target-vmaf 93 --output corpus.jsonl`

	cmd.Flags().StringArrayVar(&flags.sources, "source", nil,
		"Reference video (repeatable); required unless --from-corpus is used")
	cmd.Flags().IntVar(&flags.width, "width", 0, "Raw-YUV reference width")
	cmd.Flags().IntVar(&flags.height, "height", 0, "Raw-YUV reference height")
	cmd.Flags().StringVar(&flags.pixFmt, "pix-fmt", "yuv420p", "ffmpeg pix_fmt")
	cmd.Flags().Float64Var(&flags.framerate, "framerate", 24.0, "Reference framerate")
	cmd.Flags().Float64Var(&flags.duration, "duration", 0.0,
		"Clip duration in seconds; bounds the encode and derives achieved kbps")
	cmd.Flags().StringVar(&flags.encoder, "encoder", "libx264",
		"Codec adapter ("+strings.Join(codecadapter.Known(), ", ")+")")
	cmd.Flags().StringArrayVar(&flags.presets, "preset", nil,
		"Encoder preset (repeatable); required unless --from-corpus is used")
	cmd.Flags().StringVar(&flags.output, "output", "corpus.jsonl",
		"JSONL destination for the visited points")
	cmd.Flags().StringVar(&flags.encodeDir, "encode-dir", ".workingdir2/encodes",
		"Scratch directory for the probe encodes")
	cmd.Flags().BoolVar(&flags.keepEncodes, "keep-encodes", false,
		"Keep the encoded artefacts instead of deleting them after scoring")
	cmd.Flags().StringVar(&flags.vmafModel, "vmaf-model", "vmaf_v0.6.1",
		"libvmaf model version or path=... string")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg", "ffmpeg binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf-bin", "vmaf", "libvmaf CLI binary")
	cmd.Flags().StringVar(&flags.scoreBackend, "score-backend", "auto",
		"libvmaf scoring backend: auto, cpu, cuda, sycl, hip")
	cmd.Flags().BoolVar(&flags.noSourceHash, "no-source-hash", false,
		"Skip the source SHA-256 (faster on very large sources)")

	cmd.Flags().BoolVar(&flags.coarseToFine, "coarse-to-fine", false,
		"Run the 2-pass coarse-then-fine CRF search (always on for recommend)")
	cmd.Flags().IntVar(&flags.coarseStep, "coarse-step", 10,
		"CRF step for the coarse pass (10 -> 10,20,30,40,50)")
	cmd.Flags().IntVar(&flags.fineRadius, "fine-radius", 5,
		"Radius around the best-coarse CRF for the fine pass")
	cmd.Flags().IntVar(&flags.fineStep, "fine-step", 1, "CRF step for the fine pass")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", math.NaN(),
		"Target VMAF score; the smallest CRF whose score meets it wins")

	cmd.Flags().BoolVar(&flags.withUncertainty, "with-uncertainty", false,
		"Consume conformal prediction intervals when picking the CRF (ADR-0279)")
	cmd.Flags().StringVar(&flags.uncertaintySidecar, "uncertainty-sidecar", "",
		"Calibration sidecar JSON; defaults to the Research-0067 floor (2.0 / 5.0 VMAF)")

	cmd.Flags().StringVar(&flags.fromCorpus, "from-corpus", "",
		"Pick from an existing corpus JSONL instead of running new encodes")
	cmd.Flags().Float64Var(&flags.targetBitrate, "target-bitrate", math.NaN(),
		"With --from-corpus: pick the row whose bitrate is closest to this (kbps)")
	cmd.Flags().BoolVar(&flags.jsonOutput, "json", false,
		"Emit the recommendation as a single JSON object on stdout")

	return cmd
}

// runRecommend dispatches to the corpus-pick or the encode-driven path.
func runRecommend(ctx context.Context, d deps, flags *recommendFlags) error {
	if flags.fromCorpus != "" {
		return runRecommendFromCorpus(ctx, d, flags)
	}
	return runRecommendFromEncodes(ctx, d, flags)
}

// optionalFloat returns nil for the NaN sentinel the flags use for "unset".
func optionalFloat(v float64) *float64 {
	if math.IsNaN(v) {
		return nil
	}
	return &v
}

// runRecommendFromCorpus picks a recommendation from a pre-built corpus.
func runRecommendFromCorpus(ctx context.Context, d deps, flags *recommendFlags) error {
	rows, err := recommend.LoadCorpusJSONL(flags.fromCorpus)
	if err != nil {
		return err
	}

	targetVMAF := optionalFloat(flags.targetVMAF)
	targetBitrate := optionalFloat(flags.targetBitrate)
	if targetVMAF != nil && targetBitrate != nil {
		return errors.New("--target-vmaf and --target-bitrate are mutually exclusive")
	}

	preset := ""
	if len(flags.presets) > 0 {
		preset = flags.presets[0]
	}

	withUncertainty := flags.withUncertainty
	if withUncertainty && targetBitrate != nil {
		// No interval-aware bitrate predicate exists, so fall through to the
		// point estimate rather than silently ignoring one of the two flags.
		d.Log.WarnContext(ctx,
			"--with-uncertainty is not supported with --target-bitrate; "+
				"falling back to the point estimate")
		withUncertainty = false
	}

	if withUncertainty && targetVMAF != nil {
		thresholds := uncertainty.LoadThresholds(flags.uncertaintySidecar, d.Log)
		result, pickErr := recommend.PickTargetVMAFWithUncertainty(rows,
			recommend.UncertaintyRequest{
				TargetVMAF: *targetVMAF,
				Thresholds: thresholds,
				Encoder:    flags.encoder,
				Preset:     preset,
			})
		if pickErr != nil {
			return pickErr
		}
		if flags.jsonOutput {
			return emitRowJSON(result.Row)
		}
		status := "OK"
		if result.Margin < 0 {
			status = "UNMET"
		}
		_, printErr := fmt.Printf(
			"crf=%v  vmaf=%.3f  kbps=%.0f  predicate=%s  decision=%s  visited=%d/%d  [%s]\n",
			result.Row["crf"], rowFloat(result.Row, "vmaf_score"),
			rowFloat(result.Row, "bitrate_kbps"), result.Predicate,
			result.Decision, result.Visited, len(rows), status)
		return printErr
	}

	pick, pickErr := recommend.Recommend(rows, recommend.Request{
		TargetVMAF:        targetVMAF,
		TargetBitrateKbps: targetBitrate,
		Encoder:           flags.encoder,
		Preset:            preset,
	})
	if pickErr != nil {
		return pickErr
	}
	if flags.jsonOutput {
		return emitRowJSON(pick.Row)
	}
	status := "OK"
	if pick.Margin < 0 {
		status = "UNMET"
	}
	_, printErr := fmt.Printf("crf=%v  vmaf=%.3f  kbps=%.0f  predicate=%s  [%s]\n",
		pick.Row["crf"], rowFloat(pick.Row, "vmaf_score"),
		rowFloat(pick.Row, "bitrate_kbps"), pick.Predicate, status)
	return printErr
}

// rowFloat reads a numeric row field, returning NaN when absent.
func rowFloat(row recommend.Row, key string) float64 {
	v, ok := row[key]
	if !ok {
		return math.NaN()
	}
	f, ok := v.(float64)
	if !ok {
		return math.NaN()
	}
	return f
}

// emitRowJSON writes one row as a single-line JSON object, using Python's
// json.dumps separators and float rendering.
//
// One deliberate divergence: the keys come out sorted. The Python handler
// echoes the row in the order json.loads read it from the corpus file, but a
// Go map has no insertion order to preserve, and a stable sorted output is
// worth more than an order this side cannot reproduce. The content is
// identical, and no JSON consumer depends on member order.
func emitRowJSON(row recommend.Row) error {
	blob, err := pyjson.Marshal(map[string]any(row), pyjson.Options{SortKeys: true})
	if err != nil {
		return fmt.Errorf("marshal recommendation: %w", err)
	}
	_, printErr := fmt.Println(string(blob))
	return printErr
}

// runRecommendFromEncodes runs the coarse-to-fine sweep and picks from it.
func runRecommendFromEncodes(ctx context.Context, d deps, flags *recommendFlags) error {
	if len(flags.sources) == 0 || flags.width <= 0 || flags.height <= 0 ||
		len(flags.presets) == 0 {
		return errors.New(
			"--source, --width, --height and --preset are required unless " +
				"--from-corpus is used")
	}
	targetVMAF := optionalFloat(flags.targetVMAF)
	if targetVMAF == nil {
		return errors.New("recommend requires --target-vmaf")
	}
	adapter, adapterErr := codecadapter.Get(flags.encoder)
	if adapterErr != nil {
		return adapterErr
	}
	for _, preset := range flags.presets {
		if !adapter.HasPreset(preset) {
			return fmt.Errorf("unknown %s preset %q; expected one of %v",
				adapter.Name, preset, adapter.Presets)
		}
	}

	backend := flags.scoreBackend
	if backend == "auto" || backend == "" {
		// The libvmaf CLI picks its own default when --backend is omitted;
		// "auto" is the CLI-level spelling of that.
		backend = ""
	}
	d.Log.InfoContext(ctx, "starting coarse-to-fine recommend sweep",
		"sources", flags.sources, "presets", flags.presets,
		"encoder", flags.encoder, "target_vmaf", *targetVMAF,
		"score_backend", flags.scoreBackend)

	var visited []recommend.Row
	for _, source := range flags.sources {
		rows, err := sweepOneSource(ctx, d, flags, source, backend)
		if err != nil {
			return err
		}
		visited = append(visited, rows...)
	}

	corpusRows := make([]corpusrow.Row, len(visited))
	for i, r := range visited {
		corpusRows[i] = corpusrow.Row(r)
	}
	if _, err := corpusrow.WriteJSONL(corpusRows, flags.output); err != nil {
		return err
	}

	if flags.withUncertainty {
		thresholds := uncertainty.LoadThresholds(flags.uncertaintySidecar, d.Log)
		result, pickErr := recommend.PickTargetVMAFWithUncertainty(visited,
			recommend.UncertaintyRequest{
				TargetVMAF: *targetVMAF, Thresholds: thresholds,
			})
		if pickErr != nil {
			return fmt.Errorf(
				"uncertainty pick failed (%w); visited %d encodes -> %s",
				pickErr, len(visited), flags.output)
		}
		_, printErr := fmt.Printf(
			"src=%v preset=%v crf=%v vmaf=%.3f decision=%s visited=%d/%d predicate=%s\n",
			result.Row["src"], result.Row["preset"], result.Row["crf"],
			rowFloat(result.Row, "vmaf_score"), result.Decision,
			result.Visited, len(visited), result.Predicate)
		return printErr
	}

	src, preset, crf, score, ok := recommend.SmallestPassingCRF(visited, *targetVMAF)
	if !ok {
		return fmt.Errorf(
			"no CRF meets target VMAF >= %g; visited %d encodes -> %s",
			*targetVMAF, len(visited), flags.output)
	}
	_, printErr := fmt.Printf("src=%s preset=%s crf=%d vmaf=%.3f (visited %d encodes)\n",
		src, preset, crf, score, len(visited))
	return printErr
}

// sweepOneSource runs the coarse-to-fine sweep for a single source.
func sweepOneSource(
	ctx context.Context,
	d deps,
	flags *recommendFlags,
	source, backend string,
) ([]recommend.Row, error) {
	if _, err := os.Stat(source); err != nil {
		return nil, fmt.Errorf("source %q: %w", source, err)
	}
	srcHash := ""
	if !flags.noSourceHash {
		hash, err := sha256File(source)
		if err != nil {
			return nil, err
		}
		srcHash = hash
	}

	job := corpusrow.Job{
		Source: source, Width: flags.width, Height: flags.height,
		PixFmt: flags.pixFmt, Framerate: flags.framerate,
		DurationS: flags.duration, SrcSHA256: srcHash,
	}
	opts := corpusrow.Options{
		Encoder: flags.encoder, EncodeDir: flags.encodeDir,
		VMAFModel: flags.vmafModel, FFmpegBin: flags.ffmpegBin,
		VMAFBin: flags.vmafBin, KeepEncodes: flags.keepEncodes,
		ScoreBackend: backend,
	}

	runCell := func(cellCtx context.Context, preset string, crf int) (corpusrow.Row, error) {
		return runOneCell(cellCtx, d, flags, job, opts, preset, crf, backend)
	}

	// The CRF window stays at the Python coarse_to_fine_search defaults
	// (10..50) rather than the adapter's own quality range. The Python CLI
	// never overrides them either, so a Go and a Python run over the same
	// source visit the same cells and their corpora stay comparable. CRF
	// below 10 is visually lossless on most content and 51 is the codec
	// floor, so the window is the practically useful part of the axis.
	searchOpts := corpusrow.DefaultSearchOptions()
	searchOpts.Presets = flags.presets
	searchOpts.TargetVMAF = optionalFloat(flags.targetVMAF)
	searchOpts.CoarseStep = flags.coarseStep
	searchOpts.FineRadius = flags.fineRadius
	searchOpts.FineStep = flags.fineStep

	rows, err := corpusrow.CoarseToFineSearch(ctx, runCell, searchOpts)
	if err != nil {
		return nil, err
	}
	out := make([]recommend.Row, len(rows))
	for i, r := range rows {
		out[i] = recommend.Row(r)
	}
	return out, nil
}

// runOneCell encodes one (preset, crf) cell, scores it, and builds its row.
//
// A failed encode or score is recorded in the row's exit_status with a NaN
// score rather than aborting: the search must survive a single bad cell, and
// the row filter drops non-zero exit statuses at pick time.
func runOneCell(
	ctx context.Context,
	d deps,
	flags *recommendFlags,
	job corpusrow.Job,
	opts corpusrow.Options,
	preset string,
	crf int,
	backend string,
) (corpusrow.Row, error) {
	// G301: 0o750 keeps the scratch dir owner+group accessible only.
	if err := os.MkdirAll(flags.encodeDir, 0o750); err != nil {
		return nil, fmt.Errorf("create encode dir: %w", err)
	}
	outPath := filepath.Join(flags.encodeDir,
		fmt.Sprintf("%s__%s__%s__crf%d.mp4",
			sanitiseStem(filepath.Base(job.Source)), flags.encoder, preset, crf))

	encReq := ffencode.Request{
		Source: job.Source, Width: job.Width, Height: job.Height,
		PixFmt: job.PixFmt, Framerate: job.Framerate,
		Encoder: flags.encoder, Preset: preset, CRF: crf,
		Output: outPath, DurationS: job.DurationS,
		SourceIsContainer: scorecli.NeedsDecode(job.Source),
	}
	encRes, encErr := ffencode.Run(ctx, encReq, flags.ffmpegBin, nil)
	if encErr != nil {
		return nil, encErr
	}
	if !flags.keepEncodes {
		defer func() {
			if rmErr := os.Remove(outPath); rmErr != nil && !os.IsNotExist(rmErr) {
				d.Log.WarnContext(ctx, "remove probe encode",
					"path", outPath, "error", rmErr)
			}
		}()
	}

	scoreRes := scorecli.Result{VMAFScore: math.NaN(), ExitStatus: encRes.ExitStatus}
	if encRes.ExitStatus == 0 {
		var scoreErr error
		scoreRes, scoreErr = scoreEncode(ctx, d, flags, job, outPath, backend)
		if scoreErr != nil {
			return nil, scoreErr
		}
	} else {
		d.Log.WarnContext(ctx, "encode failed; recording a NaN score for the cell",
			"preset", preset, "crf", crf, "exit_status", encRes.ExitStatus,
			"stderr_tail", encRes.StderrTail)
	}

	return corpusrow.NewRow(job, opts, preset, crf, encRes, scoreRes, flags.vmafModel), nil
}

// scoreEncode scores one encoded artefact against the reference, decoding the
// distorted side to raw YUV first when the container demands it.
func scoreEncode(
	ctx context.Context,
	d deps,
	flags *recommendFlags,
	job corpusrow.Job,
	encodedPath, backend string,
) (scorecli.Result, error) {
	distorted := encodedPath
	if scorecli.NeedsDecode(encodedPath) {
		decoded := strings.TrimSuffix(encodedPath, filepath.Ext(encodedPath)) + ".decoded.yuv"
		argv := scorecli.DecodeCommand(
			encodedPath, decoded, job.PixFmt, flags.ffmpegBin, job.DurationS)
		_, _, exitStatus, err := runCommand(ctx, argv)
		if err != nil || exitStatus != 0 {
			d.Log.WarnContext(ctx, "decode of the distorted leg failed; "+
				"recording a NaN score", "path", encodedPath, "exit_status", exitStatus)
			return scorecli.Result{VMAFScore: math.NaN(), ExitStatus: 1}, nil
		}
		defer func() {
			if rmErr := os.Remove(decoded); rmErr != nil && !os.IsNotExist(rmErr) {
				d.Log.WarnContext(ctx, "remove decoded sidecar",
					"path", decoded, "error", rmErr)
			}
		}()
		distorted = decoded
	}

	return scorecli.Run(ctx, scorecli.Request{
		Reference: job.Source, Distorted: distorted,
		Width: job.Width, Height: job.Height, PixFmt: job.PixFmt,
		Model: flags.vmafModel, DurationS: job.DurationS,
	}, flags.vmafBin, backend, nil)
}

// sanitiseStem strips the extension and replaces path separators so the stem
// is safe as a filename component.
func sanitiseStem(name string) string {
	stem := strings.TrimSuffix(name, filepath.Ext(name))
	return strings.NewReplacer("/", "_", "\\", "_", " ", "_").Replace(stem)
}

// sha256File returns the hex SHA-256 of the file at path.
func sha256File(path string) (string, error) {
	// #nosec G304 -- path is an operator-supplied --source flag.
	fh, err := os.Open(path)
	if err != nil {
		return "", fmt.Errorf("hash %q: %w", path, err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			_ = closeErr
		}
	}()
	h := sha256.New()
	if _, copyErr := io.Copy(h, fh); copyErr != nil {
		return "", fmt.Errorf("hash %q: %w", path, copyErr)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
