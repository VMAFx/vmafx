// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package cmd

import (
	"context"
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/scorebackend"
)

// perShotFlags holds flags parsed by the tune-per-shot subcommand.
//
// Flag names, defaults and help text mirror the Python
// `vmaf-tune tune-per-shot` parser one-for-one so an operator's existing
// command lines keep working against the Go binary.
type perShotFlags struct {
	src               string
	width             int
	height            int
	pixFmt            string
	framerate         float64
	targetVMAF        float64
	enc               string
	bitdepth          int
	totalFrames       int
	sceneThreshold    float64
	maxShotDuration   float64
	perShotBin        string
	ffmpegBin         string
	vmafBin           string
	preset            string
	crfMin            int
	crfMax            int
	maxIterations     int
	vmafModel         string
	neg               bool
	fastNR            bool
	scoreBackend      string
	predicateModule   string
	output            string
	segmentDir        string
	planOut           string
	scriptOut         string
	workDir           string
	maxConcurrentDecs int
}

// perShotSentinelCRF marks "--crf-min / --crf-max not supplied". The Python
// defaults both to None; a Go int flag needs an out-of-band value, and -1 is
// outside every codec's quality window.
const perShotSentinelCRF = -1

// newPerShotCmd builds and returns the "tune-per-shot" cobra subcommand.
func newPerShotCmd() *cobra.Command {
	flags := &perShotFlags{}

	cmd := clikit.Command("tune-per-shot",
		"Per-shot CRF tuning: detect shots, bisect each, emit an FFmpeg encoding plan",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runPerShot(ctx, d, flags)
		})),
	)
	cmd.Long = `Detect shot boundaries with vmaf-perShot (TransNet V2), run a CRF
bisect against the VMAF target inside each shot, and emit an FFmpeg encoding
plan that produces a per-shot CRF-varying encode plus the concat command that
stitches the segments together.

The plan is emitted, not executed: inspect it, or run the segment commands
yourself (they are independent and safe to parallelise) followed by the
concat command.

Geometry is auto-probed with ffprobe for container sources (mp4, mkv, ...).
Raw YUV sources (.yuv / .raw) do not describe themselves, so --width and
--height are required for those.

Two flags of the Python parser have no Go implementation and fail fast rather
than being silently ignored:
  --predicate-module   loads a Python callable at runtime; Go has no
                       equivalent. The Go seam is pershot.PredicateFn.
  --fast-nr            needs onnxruntime for the nr_metric_v1 NR proxy.
Use 'vmaf-tune tune-per-shot' for either.

Example:
  vmafx-tune-go tune-per-shot \
    --src src.mp4 \
    --target-vmaf 92 \
    --encoder libx264 \
    --plan-out plan.json`

	cmd.Flags().StringVar(&flags.src, "src", "",
		"reference video (raw YUV or any FFmpeg-readable container) (required)")
	cmd.Flags().IntVar(&flags.width, "width", 0,
		"source width. Required for raw YUV (.yuv/.raw) sources; auto-probed "+
			"from ffprobe for container sources when omitted (ADR-0548)")
	cmd.Flags().IntVar(&flags.height, "height", 0,
		"source height. Required for raw YUV sources; auto-probed for "+
			"container sources when omitted (ADR-0548)")
	cmd.Flags().StringVar(&flags.pixFmt, "pix-fmt", "yuv420p",
		"source pixel format")
	cmd.Flags().Float64Var(&flags.framerate, "framerate", 0,
		"source framerate. Auto-probed for container sources when omitted; "+
			"defaults to 24.0 if the probe cannot determine a rate (ADR-0548)")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", 92.0,
		"target pooled-mean VMAF for the per-shot predicate")
	cmd.Flags().StringVar(&flags.enc, "encoder", "libx264",
		"codec adapter: "+strings.Join(encoder.KnownAdapters(), ", "))
	cmd.Flags().IntVar(&flags.bitdepth, "bitdepth", 8,
		"source YUV bit depth (8, 10 or 12; forwarded to vmaf-perShot)")
	cmd.Flags().IntVar(&flags.totalFrames, "total-frames", 0,
		"frame count for the single-shot fallback (used when vmaf-perShot is unavailable)")
	cmd.Flags().Float64Var(&flags.sceneThreshold, "scene-threshold", math.NaN(),
		"override vmaf-perShot --diff-threshold (mean-absolute-luma-delta cutoff "+
			"for cut classification; lower = more shots). Omit to keep the C-side "+
			"compiled default of 12.0 on 8-bit content (ADR-0513)")
	cmd.Flags().Float64Var(&flags.maxShotDuration, "max-shot-duration", 2.0,
		"uniform-time-window splitter: any detected shot longer than this many "+
			"seconds is sliced into equal-length sub-shots so the tuner sees a "+
			"non-degenerate timeline even when the detector under-cuts. "+
			"Set to 0 to disable (ADR-0513)")
	cmd.Flags().StringVar(&flags.perShotBin, "per-shot-bin", pershot.DefaultPerShotBin,
		"path to the vmaf-perShot binary")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg",
		"path to the ffmpeg binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf-bin", "vmaf",
		"path to the vmaf binary used by the per-shot bisect scorer")
	cmd.Flags().StringVar(&flags.preset, "preset", "",
		"codec preset forwarded to the per-shot bisect encodes "+
			"(default: the codec adapter's mid-range preset)")
	cmd.Flags().IntVar(&flags.crfMin, "crf-min", perShotSentinelCRF,
		"inclusive lower CRF bound for the bisect search window")
	cmd.Flags().IntVar(&flags.crfMax, "crf-max", perShotSentinelCRF,
		"inclusive upper CRF bound for the bisect search window")
	cmd.Flags().IntVar(&flags.maxIterations, "max-iterations", 8,
		"maximum encode+score iterations per detected shot")
	cmd.Flags().StringVar(&flags.vmafModel, "vmaf-model", "vmaf_v0.6.1",
		"VMAF model name forwarded to the per-shot bisect scorer")
	cmd.Flags().BoolVar(&flags.neg, "neg", false,
		"use the VMAF NEG (No Enhancement Gain) model variant, which penalises "+
			"sharpening-based score inflation. Routes --vmaf-model vmaf_v0.6.1 to "+
			"vmaf_v0.6.1neg (or the 4K equivalent). Use for codec A vs B "+
			"comparisons; do NOT use for production quality monitoring against "+
			"baselines. See docs/metrics/vmaf-neg.md (ADR-0622)")
	cmd.Flags().BoolVar(&flags.fastNR, "fast-nr", false,
		"NOT IMPLEMENTED in the Go port: NR early-elimination requires "+
			"onnxruntime. Use 'vmaf-tune tune-per-shot --fast-nr'")
	cmd.Flags().StringVar(&flags.scoreBackend, "score-backend", "auto",
		"libvmaf score backend for the per-shot bisect scorer: auto, "+
			strings.Join(scorebackend.All, ", "))
	cmd.Flags().StringVar(&flags.predicateModule, "predicate-module", "",
		"NOT IMPLEMENTED in the Go port: loading a Python MODULE:CALLABLE has no "+
			"Go equivalent. Use 'vmaf-tune tune-per-shot --predicate-module'")
	cmd.Flags().StringVar(&flags.output, "output", "per_shot_encode.mp4",
		"final concatenated encode destination named in the emitted plan")
	cmd.Flags().StringVar(&flags.segmentDir, "segment-dir", "",
		"directory for per-shot segment files (default: <output dir>/segments)")
	cmd.Flags().StringVar(&flags.planOut, "plan-out", "",
		"emit the JSON plan to this path (default: stdout)")
	cmd.Flags().StringVar(&flags.scriptOut, "script-out", "",
		"optional: write a copy-paste shell script of the plan")
	cmd.Flags().StringVar(&flags.workDir, "workdir", "",
		"directory for temporary bisect encode / decode artefacts. Overrides "+
			"VMAFTUNE_WORKDIR and the OS temp default. Ensure the volume has "+
			"sufficient free space for raw YUV decodes (ADR-0598)")
	cmd.Flags().IntVar(&flags.maxConcurrentDecs, "max-concurrent-decodes", 1,
		"maximum number of reference-YUV decode operations that may run "+
			"simultaneously (ADR-0577). Default 1 (serial decodes) — safest for "+
			"disk-space constrained volumes")

	_ = cmd.MarkFlagRequired("src")

	return cmd
}

// runPerShot is the implementation of the tune-per-shot subcommand.
func runPerShot(ctx context.Context, d deps, flags *perShotFlags) error {
	if err := rejectUnportedPerShotFlags(flags); err != nil {
		return err
	}
	if flags.src == "" {
		return errors.New("--src is required")
	}
	if _, err := os.Stat(flags.src); err != nil {
		return fmt.Errorf("source file %q: %w", flags.src, err)
	}
	if flags.targetVMAF <= 0 || flags.targetVMAF > 100 {
		return fmt.Errorf("--target-vmaf %g is out of range (0, 100]", flags.targetVMAF)
	}
	if flags.maxIterations <= 0 {
		return fmt.Errorf("--max-iterations must be positive, got %d", flags.maxIterations)
	}
	if flags.maxConcurrentDecs < 1 {
		return fmt.Errorf("--max-concurrent-decodes must be >= 1, got %d",
			flags.maxConcurrentDecs)
	}
	switch flags.bitdepth {
	case 8, 10, 12:
	default:
		return fmt.Errorf("--bitdepth must be 8, 10 or 12, got %d", flags.bitdepth)
	}
	adapter, adapterErr := encoder.GetAdapter(flags.enc)
	if adapterErr != nil {
		return fmt.Errorf("--encoder: %w", adapterErr)
	}
	if flags.preset != "" && !adapter.HasPreset(flags.preset) {
		return fmt.Errorf("--preset %q is not a %s preset; expected one of %v",
			flags.preset, adapter.Name, adapter.Presets)
	}
	crfRange, crfErr := parseOptionalCRFRange(flags.crfMin, flags.crfMax)
	if crfErr != nil {
		return crfErr
	}

	// ADR-0667: resolve --score-backend before touching any source file or
	// launching a bisect. An unavailable backend must fail fast here with an
	// actionable message rather than surfacing as a cryptic vmaf error buried
	// inside the first shot's bisect loop.
	backend, backendErr := scorebackend.Select(ctx, flags.scoreBackend,
		scorebackend.Options{VMAFBin: flags.vmafBin})
	if backendErr != nil {
		return backendErr
	}
	d.Log.InfoContext(ctx, "per-shot scoring backend resolved",
		"requested", flags.scoreBackend, "backend", backend)

	geom, geomErr := resolvePerShotGeometry(flags)
	if geomErr != nil {
		return geomErr
	}
	d.Log.InfoContext(ctx, "per-shot source geometry",
		"src", flags.src,
		"width", geom.width, "height", geom.height,
		"framerate", geom.framerate, "total_frames", geom.totalFrames)

	// ADR-0513: thread the scene threshold and the uniform-window splitter
	// through so short clips and under-cutting content still produce a
	// multi-shot timeline.
	detectOpts := pershot.DetectOptions{
		Width:              geom.width,
		Height:             geom.height,
		PixFmt:             flags.pixFmt,
		Bitdepth:           flags.bitdepth,
		TotalFrames:        geom.totalFrames,
		Bin:                flags.perShotBin,
		Framerate:          geom.framerate,
		MaxShotDurationSec: flags.maxShotDuration,
	}
	if !math.IsNaN(flags.sceneThreshold) {
		threshold := flags.sceneThreshold
		detectOpts.DiffThreshold = &threshold
	}
	shots := pershot.DetectShots(ctx, flags.src, detectOpts)
	d.Log.InfoContext(ctx, "shot detection complete", "shots", len(shots))

	scratch, scratchErr := os.MkdirTemp(perShotWorkdirParent(flags.workDir),
		"vmafx-tune-per-shot-")
	if scratchErr != nil {
		return fmt.Errorf("create per-shot scratch dir: %w", scratchErr)
	}
	defer func() {
		if rmErr := os.RemoveAll(scratch); rmErr != nil {
			d.Log.WarnContext(ctx, "per-shot scratch cleanup failed",
				"error", rmErr, "path", scratch)
		}
	}()

	predicate, sidecar, predErr := newBisectPredicate(bisectPredicateConfig{
		flags:    flags,
		geom:     geom,
		adapter:  adapter,
		backend:  backend,
		crfRange: crfRange,
		scratch:  scratch,
	})
	if predErr != nil {
		return predErr
	}

	recs, tuneErr := pershot.Tune(shots, pershot.TuneParams{
		TargetVMAF: flags.targetVMAF,
		Encoder:    flags.enc,
		Predicate:  predicate,
	})
	if tuneErr != nil {
		return tuneErr
	}
	// ADR-0536: attach the bitrates the bisect measured, keyed by frame range.
	recs = pershot.WithBitrates(recs, sidecar)

	plan, mergeErr := pershot.Merge(recs, pershot.MergeParams{
		Source:     flags.src,
		Output:     flags.output,
		Framerate:  geom.framerate,
		Encoder:    flags.enc,
		SegmentDir: flags.segmentDir,
		FFmpegBin:  flags.ffmpegBin,
	})
	if mergeErr != nil {
		return mergeErr
	}

	rendered, renderErr := pershot.RenderPlanJSON(plan, "bisect", flags.targetVMAF)
	if renderErr != nil {
		return renderErr
	}
	if err := writeOutput(flags.planOut, rendered+"\n"); err != nil {
		return err
	}
	if flags.planOut != "" {
		d.Log.InfoContext(ctx, "wrote per-shot plan", "path", flags.planOut)
	}

	if flags.scriptOut != "" {
		if err := writeOutput(flags.scriptOut, pershot.PlanToShellScript(plan)); err != nil {
			return err
		}
		d.Log.InfoContext(ctx, "wrote per-shot shell script", "path", flags.scriptOut)
	}

	// Concat-listing destination. Prefer --segment-dir, then the directory
	// holding --plan-out (writable by construction — the plan JSON just
	// landed there), and only then <output dir>/segments, which resolves
	// against the CWD and may be read-only inside a bind-mounted container
	// workspace (ADR-0532).
	//
	// Note this can differ from plan.SegmentDir, which the segment commands
	// were built against: the Python has the same split, and matching it
	// keeps operational parity for existing runbooks. When the two diverge
	// the divergence is logged rather than silently papered over.
	listingDir := plan.SegmentDir
	if flags.segmentDir == "" && flags.planOut != "" {
		listingDir = filepath.Join(filepath.Dir(flags.planOut), "segments")
	}
	if listingDir != plan.SegmentDir {
		d.Log.WarnContext(ctx,
			"concat listing lands beside --plan-out, not in the plan's segment dir; "+
				"pass --segment-dir to pin both",
			"listing_dir", listingDir, "plan_segment_dir", plan.SegmentDir)
	}
	if err := pershot.WriteConcatListing(plan, filepath.Join(listingDir, "concat.txt")); err != nil {
		// A read-only segments dir must not lose the plan the run just
		// produced; warn and continue, matching the Python.
		d.Log.WarnContext(ctx, "segments dir not writable; skipping concat listing",
			"error", err, "dir", listingDir)
	}
	return nil
}

// rejectUnportedPerShotFlags fails fast on the two flags with no Go
// implementation, naming the Python fallback. Accepting and ignoring them
// would silently change the run's semantics.
func rejectUnportedPerShotFlags(flags *perShotFlags) error {
	if flags.predicateModule != "" {
		return fmt.Errorf(
			"--predicate-module %q is not supported by vmafx-tune-go: the flag "+
				"imports a Python callable at runtime, which Go cannot do. The Go "+
				"equivalent is the pershot.PredicateFn seam (library callers only). "+
				"Run 'vmaf-tune tune-per-shot --predicate-module %s' for this workflow",
			flags.predicateModule, flags.predicateModule)
	}
	if flags.fastNR {
		return errors.New(
			"--fast-nr is not supported by vmafx-tune-go: NR early-elimination runs " +
				"the nr_metric_v1 ONNX model through onnxruntime, for which this " +
				"binary has no runtime binding. Run 'vmaf-tune tune-per-shot " +
				"--fast-nr' for this workflow")
	}
	return nil
}

// parseOptionalCRFRange validates the optional --crf-min / --crf-max pair.
// Returns nil when neither was supplied, so the bisect falls back to the
// codec's absolute quality window (ADR-0538). Mirrors
// cli._parse_optional_crf_range.
func parseOptionalCRFRange(crfMin, crfMax int) (*[2]int, error) {
	if crfMin == perShotSentinelCRF && crfMax == perShotSentinelCRF {
		return nil, nil
	}
	if crfMin == perShotSentinelCRF || crfMax == perShotSentinelCRF {
		return nil, errors.New("pass both --crf-min and --crf-max")
	}
	if crfMin > crfMax {
		return nil, fmt.Errorf("invalid CRF range [%d, %d]", crfMin, crfMax)
	}
	return &[2]int{crfMin, crfMax}, nil
}

// perShotGeometry is the resolved source geometry the run operates on.
type perShotGeometry struct {
	width       int
	height      int
	framerate   float64
	totalFrames int
}

// sourceNeedsRawvideoDemux reports whether src is an extension-only raw YUV
// input, which ffmpeg cannot probe and must be told the geometry for.
// Mirrors cli._source_needs_rawvideo_demux.
func sourceNeedsRawvideoDemux(src string) bool {
	switch strings.ToLower(filepath.Ext(src)) {
	case ".yuv", ".raw":
		return true
	default:
		return false
	}
}

// resolvePerShotGeometry fills in width / height / framerate / frame count,
// auto-probing container sources with ffprobe (ADR-0548) and requiring
// explicit geometry for raw YUV.
func resolvePerShotGeometry(flags *perShotFlags) (perShotGeometry, error) {
	geom := perShotGeometry{
		width:       flags.width,
		height:      flags.height,
		framerate:   flags.framerate,
		totalFrames: flags.totalFrames,
	}
	if geom.totalFrames < 0 {
		geom.totalFrames = 0
	}

	if sourceNeedsRawvideoDemux(flags.src) {
		if geom.width <= 0 || geom.height <= 0 {
			return perShotGeometry{}, errors.New(
				"--width and --height are required for raw YUV sources. For " +
					"container sources (mp4, mkv, ...) they are optional and " +
					"auto-probed via ffprobe")
		}
	} else if geom.width <= 0 || geom.height <= 0 || geom.framerate <= 0 {
		info := encoder.ProbeSource(flags.src, flags.ffmpegBin)
		if geom.width <= 0 {
			geom.width = info.Width
		}
		if geom.height <= 0 {
			geom.height = info.Height
		}
		if geom.framerate <= 0 && info.FPS > 0 {
			geom.framerate = info.FPS
		}
		if geom.totalFrames <= 0 && info.FrameCount > 0 {
			geom.totalFrames = info.FrameCount
		}
	}

	if geom.width <= 0 || geom.height <= 0 {
		return perShotGeometry{}, errors.New(
			"could not determine source width/height. Pass --width and --height explicitly")
	}
	if geom.framerate <= 0 {
		// Safe default when the probe yields nothing, matching the Python.
		geom.framerate = 24.0
	}
	return geom, nil
}

// perShotWorkdirParent resolves the parent directory for the run's scratch
// tree: the explicit --workdir, then VMAFTUNE_WORKDIR when it is writable,
// then "" (the OS temp default). Mirrors bisect._workdir_parent (ADR-0598).
func perShotWorkdirParent(workDir string) string {
	if workDir != "" {
		// A MkdirAll failure is not fatal here: os.MkdirTemp reports the same
		// condition with a better message a moment later.
		_ = os.MkdirAll(workDir, 0o750)
		return workDir
	}
	env := os.Getenv("VMAFTUNE_WORKDIR")
	if env == "" {
		return ""
	}
	if err := os.MkdirAll(env, 0o750); err != nil {
		return ""
	}
	probe, err := os.CreateTemp(env, ".vmafx-tune-writable-*")
	if err != nil {
		return ""
	}
	name := probe.Name()
	_ = probe.Close()
	_ = os.Remove(name)
	return env
}

// bisectPredicateConfig bundles everything newBisectPredicate needs.
type bisectPredicateConfig struct {
	flags    *perShotFlags
	geom     perShotGeometry
	adapter  encoder.Adapter
	backend  string
	crfRange *[2]int
	scratch  string
}

// newBisectPredicate builds the production per-shot predicate from the CRF
// bisect, plus the bitrate sidecar the caller reads afterwards.
//
// pkg/bisect operates on a single file, so each shot is first extracted to a
// raw YUV reference and the bisect runs over that isolated range. The
// sidecar is keyed by shot rather than widening PredicateFn's return type
// (ADR-0536).
func newBisectPredicate(cfg bisectPredicateConfig) (
	pershot.PredicateFn, map[pershot.Shot]float64, error,
) {
	flags := cfg.flags
	refsDir := filepath.Join(cfg.scratch, "refs")
	workDir := filepath.Join(cfg.scratch, "bisect")
	for _, dir := range []string{refsDir, workDir} {
		if err := os.MkdirAll(dir, 0o750); err != nil {
			return nil, nil, fmt.Errorf("create per-shot scratch dir: %w", err)
		}
	}

	// The shot references are headerless raw YUV, so every bisect encode has
	// to be told the demuxer geometry before "-i".
	rawInputArgs := []string{
		"-f", "rawvideo",
		"-pix_fmt", flags.pixFmt,
		"-s", fmt.Sprintf("%dx%d", cfg.geom.width, cfg.geom.height),
		"-r", trimFloat(cfg.geom.framerate),
	}
	enc, encErr := encoder.NewAdapterEncoder(flags.enc, flags.preset, rawInputArgs)
	if encErr != nil {
		return nil, nil, encErr
	}

	// ADR-0577: one shared decode-slot pool for the whole run. Shots are
	// tuned serially (pershot.Tune walks them in order) and each shot's
	// bisect is itself serial, so today at most one decode is ever in
	// flight and the cap never actually binds — exactly as in the Python.
	// The pool is wired anyway so the flag stays honest and a future
	// parallel-shot scheduler inherits the bound rather than having to
	// rediscover it.
	decodeSem := make(chan struct{}, flags.maxConcurrentDecs)
	// Written only from the predicate below, which pershot.Tune calls
	// serially — no lock needed. Keyed by shot rather than widening the
	// PredicateFn return type (ADR-0536).
	sidecar := map[pershot.Shot]float64{}

	predicate := func(shot pershot.Shot, targetVMAF float64, _ string) (int, float64, error) {
		refYUV := filepath.Join(refsDir,
			fmt.Sprintf("shot_%d_%d.yuv", shot.StartFrame, shot.EndFrame))
		if err := extractShotToRawYUV(flags, cfg.geom, shot, refYUV); err != nil {
			return 0, 0, err
		}
		// Drop each shot's raw reference as soon as its bisect finishes.
		// The Python keeps every extracted shot alive until the whole run's
		// scratch dir is torn down; on a long source that is the entire
		// clip materialised as raw YUV at once, which is the disk-pressure
		// failure ADR-0577 and ADR-0598 exist to contain.
		defer func() { _ = os.Remove(refYUV) }()

		shotWorkDir := filepath.Join(workDir,
			fmt.Sprintf("shot_%d_%d", shot.StartFrame, shot.EndFrame))
		if err := os.MkdirAll(shotWorkDir, 0o750); err != nil {
			return 0, 0, fmt.Errorf("create shot workdir: %w", err)
		}

		durationS := float64(shot.Length()) / cfg.geom.framerate
		scoreFunc := bisect.YUVScoreFunc(bisect.YUVScoreParams{
			Width:     cfg.geom.width,
			Height:    cfg.geom.height,
			PixFmt:    flags.pixFmt,
			Model:     resolveVMAFModel(flags.vmafModel, flags.neg),
			Backend:   cfg.backend,
			VMAFBin:   flags.vmafBin,
			FFmpegBin: flags.ffmpegBin,
			WorkDir:   shotWorkDir,
			DurationS: durationS,
			DecodeSem: decodeSem,
		})

		params := bisect.Params{
			TargetVMAF: targetVMAF,
			MaxIter:    flags.maxIterations,
			FFmpegBin:  flags.ffmpegBin,
			WorkDir:    shotWorkDir,
		}
		if cfg.crfRange != nil {
			params.CRFLo = cfg.crfRange[0]
			params.CRFHi = cfg.crfRange[1]
		}

		result, runErr := bisect.Run(refYUV, enc, scoreFunc, params)
		if runErr != nil {
			return 0, 0, fmt.Errorf("bisect failed for shot [%d, %d): %w",
				shot.StartFrame, shot.EndFrame, runErr)
		}
		if result.BestCRF < 0 {
			return 0, 0, fmt.Errorf(
				"bisect failed for shot [%d, %d): no CRF in the search window "+
					"achieves VMAF %.1f", shot.StartFrame, shot.EndFrame, targetVMAF)
		}
		sidecar[shot] = result.BestBitratekBps
		return result.BestCRF, result.BestVMAFScore, nil
	}
	return predicate, sidecar, nil
}

// extractShotToRawYUV extracts one half-open shot range to raw YUV so the
// bisect can score it in isolation. Mirrors cli._extract_shot_to_raw_yuv.
func extractShotToRawYUV(
	flags *perShotFlags,
	geom perShotGeometry,
	shot pershot.Shot,
	output string,
) error {
	if err := os.MkdirAll(filepath.Dir(output), 0o750); err != nil {
		return fmt.Errorf("create shot refs dir: %w", err)
	}
	startSeconds := float64(shot.StartFrame) / geom.framerate

	argv := []string{"-y", "-hide_banner", "-loglevel", "error"}
	if sourceNeedsRawvideoDemux(flags.src) {
		argv = append(argv,
			"-f", "rawvideo",
			"-pix_fmt", flags.pixFmt,
			"-s", fmt.Sprintf("%dx%d", geom.width, geom.height),
			"-r", trimFloat(geom.framerate),
		)
	}
	argv = append(argv,
		"-ss", fmt.Sprintf("%.6f", startSeconds),
		"-i", flags.src,
		"-frames:v", fmt.Sprintf("%d", shot.Length()),
		"-pix_fmt", flags.pixFmt,
		"-f", "rawvideo",
		output,
	)
	ffmpegBin := flags.ffmpegBin
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	ctx, cancel := context.WithTimeout(context.Background(), shotExtractTimeout)
	defer cancel()
	// #nosec G204 -- ffmpegBin is the operator-configured --ffmpeg-bin; the
	// rest of argv is fixed flags plus --src and this call's scratch output.
	cmd := exec.CommandContext(ctx, ffmpegBin, argv...)
	out, runErr := cmd.CombinedOutput()
	if runErr != nil {
		return fmt.Errorf("ffmpeg shot extraction failed for [%d, %d): %w\n%s",
			shot.StartFrame, shot.EndFrame, runErr, lastLine(string(out)))
	}
	if _, statErr := os.Stat(output); statErr != nil {
		return fmt.Errorf("ffmpeg shot extraction produced no output for [%d, %d): %w",
			shot.StartFrame, shot.EndFrame, statErr)
	}
	return nil
}

// shotExtractTimeout bounds one shot's raw-YUV extraction. A wedged ffmpeg
// would otherwise stall the whole per-shot run indefinitely.
const shotExtractTimeout = 60 * time.Minute

// lastLine returns the final non-empty line of s, which is where ffmpeg puts
// its actual error. Falls back to a placeholder for empty output, mirroring
// the Python's "no stderr".
func lastLine(s string) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		if trimmed := strings.TrimSpace(lines[i]); trimmed != "" {
			return trimmed
		}
	}
	return "no stderr"
}

// resolveVMAFModel routes the model version through its NEG variant when
// --neg is set. Mirrors cli._resolve_vmaf_model + resolution.neg_model_for:
// a "key=value" path/version override and an already-NEG name both pass
// through unchanged, and an unknown model gets the suffix appended so
// libvmaf surfaces a clear missing-model error rather than silently using
// the wrong one.
func resolveVMAFModel(model string, neg bool) string {
	if !neg {
		return model
	}
	if strings.Contains(model, "=") {
		return model
	}
	if strings.HasSuffix(model, "neg") {
		return model
	}
	switch model {
	case "vmaf_v0.6.1":
		return "vmaf_v0.6.1neg"
	case "vmaf_4k_v0.6.1":
		return "vmaf_4k_v0.6.1neg"
	default:
		return model + "neg"
	}
}

// trimFloat renders a float with the shortest round-trip representation, for
// ffmpeg flags where a trailing ".000000" is noise.
func trimFloat(v float64) string {
	return fmt.Sprintf("%g", v)
}
