// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"github.com/VMAFx/vmafx/pkg/model"
	"math"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/prefilter"
	"github.com/VMAFx/vmafx/pkg/pyjson"
	"github.com/VMAFx/vmafx/pkg/scorecli"
)

// prefilterFlags mirrors the Python `vmaf-tune prefilter` flag surface.
type prefilterFlags struct {
	src        string
	width      int
	height     int
	pixFmt     string
	framerate  float64
	durationS  float64
	targetVMAF float64
	encoder    string
	preset     string
	filterName string
	sweepKnobs []string

	crfMin      int
	crfMax      int
	nTrials     int
	timeBudgetS float64
	seed        int64
	smoke       bool

	scoreBackend string
	ffmpegBin    string
	vmafBin      string
	vmafModel    string
	encodeDir    string
	output       string
}

// newPrefilterCmd builds the "prefilter" cobra subcommand.
func newPrefilterCmd() *cobra.Command {
	flags := &prefilterFlags{}

	cmd := clikit.Command("prefilter",
		"Control-plane autotune: joint TPE search over deband strengths + CRF",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runPrefilter(ctx, d, flags)
		})),
	)
	cmd.Long = `Autotune the Pelorus deband pre-filter jointly with the encoder CRF.

One Tree-structured Parzen Estimator study optimises all ten frozen deband
knobs (the ADR-0110 control-plane contract) and the CRF axis together, with
VMAF as the oracle. Each trial emits a deband -vf fragment, runs
deband -> encode -> score, and feeds the achieved VMAF plus bitrate back to
the sampler; the objective is |achieved - target| + lambda*kbps, so the search
converges on the lowest-bitrate combination that hits the target.

vmafx never runs the deband filter itself — it only emits the -vf string. The
live loop therefore requires the Pelorus Vulkan filter to be compiled into the
ffmpeg build, and is gated on that. Use --smoke to exercise the search against
a synthetic surface on a host without it.

SAMPLER NOTE. The Python original delegates to Optuna's TPESampler. This port
implements TPE natively (Bergstra et al. 2011 §4, the construction Optuna
follows) rather than taking a Go Optuna port that would drag gorm plus the
MySQL, Postgres and cgo-SQLite drivers into a one-shot CLI. The search space,
the objective, the JSON schema and per-seed reproducibility are identical; the
trial-by-trial trajectory for a given seed is not, and cannot be, because the
two implementations use different RNG streams.

Example:
  vmafx-tune-go prefilter --smoke --target-vmaf 93 --output rec.json

  vmafx-tune-go prefilter \
    --src ref.yuv --width 1920 --height 1080 --duration 10 \
    --target-vmaf 93 --encoder libx264 --output rec.json`

	cmd.Flags().StringVar(&flags.src, "src", "",
		"Source video; required for the live loop, optional with --smoke")
	cmd.Flags().IntVar(&flags.width, "width", 0,
		"Raw-YUV reference width (required for the live loop)")
	cmd.Flags().IntVar(&flags.height, "height", 0,
		"Raw-YUV reference height (required for the live loop)")
	cmd.Flags().StringVar(&flags.pixFmt, "pix-fmt", "yuv420p", "ffmpeg pix_fmt")
	cmd.Flags().Float64Var(&flags.framerate, "framerate", 24.0, "Reference framerate")
	cmd.Flags().Float64Var(&flags.durationS, "duration", 0.0,
		"Clip duration in seconds; 0 optimises VMAF only and reports 0 kbps, "+
			"because bitrate is undefined without a duration")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", math.NaN(),
		"Quality target on the standard VMAF [0, 100] scale (required)")
	cmd.Flags().StringVar(&flags.encoder, "encoder", "libx264",
		"Codec adapter performing the post-deband encode ("+
			strings.Join(codecadapter.Known(), ", ")+")")
	cmd.Flags().StringVar(&flags.preset, "preset", "medium",
		"Encoder preset for the probe encodes")
	cmd.Flags().StringVar(&flags.filterName, "filter", "pelorus_deband",
		"Pre-encode filter adapter to autotune ("+
			strings.Join(prefilter.KnownFilters(), ", ")+")")
	cmd.Flags().StringArrayVar(&flags.sweepKnobs, "sweep-knob", nil,
		"Restrict the deband search to this knob (repeatable); omit to sweep "+
			"all ten contract knobs: range, thry, thrc, grainy, grainc, "+
			"softness, detail, dither, dynamic, protect")
	cmd.Flags().IntVar(&flags.crfMin, "crf-min", prefilter.DefaultCRFLo,
		"Minimum CRF in the joint TPE search range")
	cmd.Flags().IntVar(&flags.crfMax, "crf-max", prefilter.DefaultCRFHi,
		"Maximum CRF in the joint TPE search range")
	cmd.Flags().IntVar(&flags.nTrials, "n-trials", 0,
		fmt.Sprintf("TPE trial budget (default %d live, %d with --smoke)",
			prefilter.DefaultNTrials, prefilter.SmokeNTrials))
	cmd.Flags().Float64Var(&flags.timeBudgetS, "time-budget-s", 600.0,
		"Soft wall-clock cap in seconds for the TPE loop")
	cmd.Flags().Int64Var(&flags.seed, "seed", 0,
		"TPE sampler seed for a reproducible search")
	cmd.Flags().BoolVar(&flags.smoke, "smoke", false,
		"Use the synthetic deband+CRF surface; no ffmpeg, Vulkan or GPU")
	cmd.Flags().StringVar(&flags.scoreBackend, "score-backend", "auto",
		"libvmaf backend for the probe scores: auto, cpu, cuda, sycl, hip")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg", "ffmpeg binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf-bin", "vmaf", "libvmaf CLI binary")
	cmd.Flags().StringVar(&flags.vmafModel, "vmaf-model", model.DefaultVersion,
		"libvmaf model version string")
	cmd.Flags().StringVar(&flags.encodeDir, "encode-dir", ".workingdir2/prefilter",
		"Scratch directory for the probe encodes")
	cmd.Flags().StringVar(&flags.output, "output", "",
		"JSON destination for the recommendation (default: stdout)")

	return cmd
}

// runPrefilter validates the flags, builds the probe and runs the search.
func runPrefilter(ctx context.Context, d deps, flags *prefilterFlags) error {
	if math.IsNaN(flags.targetVMAF) {
		return errors.New("--target-vmaf is required")
	}
	if flags.crfMin < 0 || flags.crfMax < flags.crfMin {
		return &exitCodeError{
			code: 2,
			err: fmt.Errorf("invalid CRF range [%d, %d]",
				flags.crfMin, flags.crfMax),
		}
	}

	opts := prefilter.Options{
		Src:        flags.src,
		TargetVMAF: flags.targetVMAF,
		Encoder:    flags.encoder,
		FilterName: flags.filterName,
		CRFRange:   [2]int{flags.crfMin, flags.crfMax},
		SweepKnobs: flags.sweepKnobs,
		NTrials:    flags.nTrials,
		Smoke:      flags.smoke,
		Seed:       flags.seed,
	}
	if flags.timeBudgetS > 0 {
		opts.TimeBudget = time.Duration(flags.timeBudgetS * float64(time.Second))
	}

	if !flags.smoke {
		if flags.src == "" {
			return &exitCodeError{code: 2, err: errors.New(
				"--src is required for the live loop")}
		}
		if flags.width <= 0 || flags.height <= 0 {
			return &exitCodeError{code: 2, err: errors.New(
				"--width / --height are required for the live loop (raw-YUV geometry)")}
		}
		if !prefilter.FilterAvailable(ctx, flags.ffmpegBin, prefilter.FilterName) {
			return &exitCodeError{code: 2, err: fmt.Errorf(
				"%w: %q is not in this ffmpeg build (%s). Build ffmpeg with the "+
					"Pelorus Vulkan filter, or use --smoke to exercise the search "+
					"loop without a live encode (ADR-1116 / pelorus ADR-0110)",
				prefilter.ErrFilterUnavailable, prefilter.FilterName, flags.ffmpegBin)}
		}
		backend := flags.scoreBackend
		if backend == "auto" {
			backend = ""
		}
		d.Log.InfoContext(ctx, "prefilter live loop",
			"score_backend", flags.scoreBackend, "encoder", flags.encoder)

		probe, probeErr := buildPrefilterProbe(d, flags, backend)
		if probeErr != nil {
			return &exitCodeError{code: 2, err: probeErr}
		}
		opts.Probe = probe
	}

	result, err := prefilter.RecommendPrefilter(ctx, opts)
	if err != nil {
		return &exitCodeError{code: 2, err: err}
	}

	// json.dumps(result, indent=2, sort_keys=True).
	rendered, marshalErr := pyjson.MarshalIndent(result, true)
	if marshalErr != nil {
		return fmt.Errorf("render prefilter recommendation: %w", marshalErr)
	}
	if flags.output != "" {
		if writeErr := writeOutput(flags.output, string(rendered)+"\n"); writeErr != nil {
			return writeErr
		}
		d.Log.InfoContext(ctx, "wrote prefilter recommendation", "path", flags.output)
		return nil
	}
	_, printErr := fmt.Print(string(rendered) + "\n")
	return printErr
}

// buildPrefilterProbe builds the live (deband, crf) -> ProbeResult loop.
//
// Each call emits the deband -vf fragment through the filter adapter, runs
// [deband] -> encode via the shared encode driver, scores the output against
// the source with libvmaf, and returns the achieved VMAF plus bitrate. The
// deband filter runs inside ffmpeg; vmafx only supplies the string and reads
// the score.
func buildPrefilterProbe(d deps, flags *prefilterFlags, backend string) (prefilter.ProbeFn, error) {
	adapter, err := prefilter.GetAdapter(flags.filterName)
	if err != nil {
		return nil, err
	}
	workdir := filepath.Join(flags.encodeDir, "probes")
	// G301: 0o750 keeps the probe scratch dir owner+group accessible only.
	if mkErr := os.MkdirAll(workdir, 0o750); mkErr != nil {
		return nil, fmt.Errorf("create probe workdir: %w", mkErr)
	}
	sourceIsContainer := scorecli.NeedsDecode(flags.src)

	return func(ctx context.Context, deband map[string]float64, crf int) (prefilter.ProbeResult, error) {
		fragment, fragErr := adapter.VFFragment(deband, false)
		if fragErr != nil {
			return prefilter.ProbeResult{}, fragErr
		}
		slot := filepath.Join(workdir,
			fmt.Sprintf("probe_crf%d_%06x.mp4", crf, fragmentTag(fragment)))

		encRes, encErr := ffencode.Run(ctx, ffencode.Request{
			Source: flags.src, Width: flags.width, Height: flags.height,
			PixFmt: flags.pixFmt, Framerate: flags.framerate,
			DurationS: flags.durationS,
			Encoder:   flags.encoder, Preset: flags.preset, CRF: crf,
			Output: slot,
			// The deband fragment is injected as an input filter chain ahead
			// of the encoder; extra params land after the codec args and
			// before the output.
			ExtraParams:       []string{"-vf", fragment},
			SourceIsContainer: sourceIsContainer,
		}, flags.ffmpegBin, nil)
		if encErr != nil {
			return prefilter.ProbeResult{}, encErr
		}
		defer func() {
			if rmErr := os.Remove(slot); rmErr != nil && !os.IsNotExist(rmErr) {
				d.Log.WarnContext(ctx, "remove probe encode",
					"path", slot, "error", rmErr)
			}
		}()

		if encRes.ExitStatus != 0 || encRes.EncodeSizeBytes == 0 {
			// A failed probe scores 0 VMAF so the sampler steers away from
			// that region rather than aborting the whole sweep.
			d.Log.WarnContext(ctx, "probe encode failed; scoring it 0 VMAF",
				"crf", crf, "exit_status", encRes.ExitStatus,
				"stderr_tail", encRes.StderrTail)
			return prefilter.ProbeResult{VMAF: 0.0, Kbps: 0.0, VFFragment: fragment}, nil
		}
		observedKbps := ffencode.BitrateKbps(encRes.EncodeSizeBytes, flags.durationS)

		distorted := slot
		if scorecli.NeedsDecode(slot) {
			decoded := strings.TrimSuffix(slot, filepath.Ext(slot)) + ".decoded.yuv"
			argv := scorecli.DecodeCommand(
				slot, decoded, flags.pixFmt, flags.ffmpegBin, flags.durationS)
			_, _, exitStatus, decodeErr := runCommand(ctx, argv)
			if decodeErr != nil || exitStatus != 0 {
				d.Log.WarnContext(ctx, "probe decode failed; scoring it 0 VMAF",
					"crf", crf, "exit_status", exitStatus)
				return prefilter.ProbeResult{VMAF: 0.0, Kbps: observedKbps, VFFragment: fragment}, nil
			}
			defer func() {
				if rmErr := os.Remove(decoded); rmErr != nil && !os.IsNotExist(rmErr) {
					d.Log.WarnContext(ctx, "remove decoded probe",
						"path", decoded, "error", rmErr)
				}
			}()
			distorted = decoded
		}

		scoreRes, scoreErr := scorecli.Run(ctx, scorecli.Request{
			Reference: flags.src, Distorted: distorted,
			Width: flags.width, Height: flags.height, PixFmt: flags.pixFmt,
			Model: flags.vmafModel, DurationS: flags.durationS,
		}, flags.vmafBin, backend, nil)
		if scoreErr != nil {
			return prefilter.ProbeResult{}, scoreErr
		}
		vmaf := scoreRes.VMAFScore
		if math.IsNaN(vmaf) {
			vmaf = 0.0
		}
		return prefilter.ProbeResult{
			VMAF: vmaf, Kbps: observedKbps, VFFragment: fragment,
		}, nil
	}, nil
}

// fragmentTag derives a short, stable filename tag from a -vf fragment so
// concurrent probes cannot collide on the same scratch path.
func fragmentTag(fragment string) uint32 {
	// FNV-1a, inlined to keep the tag stable across Go releases (hash/fnv is
	// stable, but writing it out makes the filename contract explicit).
	const offset32 = 2166136261
	const prime32 = 16777619
	h := uint32(offset32)
	for i := 0; i < len(fragment); i++ {
		h ^= uint32(fragment[i])
		h *= prime32
	}
	return h & 0xFFFFFF
}
