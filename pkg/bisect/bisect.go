// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package bisect implements the VMAF-target CRF bisect used by vmafx-tune
// compare.
//
// Algorithm (mirrors tools/vmaf-tune/src/vmaftune/bisect.py Phase B):
//
//  1. Encode at the midpoint CRF of the current [lo, hi] window and score
//     with the vmaf binary via subprocess.
//  2. If measured VMAF >= target, narrow the window upward (higher CRF =
//     more compression = smaller file while still meeting the target).
//  3. Else narrow downward.
//  4. Stop when the window collapses to a single integer or after MaxIter.
//
// The bisect assumes monotone-decreasing VMAF in CRF. The algorithm is
// stateless: callers supply an Encoder and a ScoreFunc, enabling clean
// unit testing via mock injections.
package bisect

import (
	"context"
	"errors"
	"fmt"
	"iter"
	"log/slog"
	"math"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/VMAFx/vmafx/pkg/encoder"
)

// defaultScoreTimeout is the upper bound on a single vmaf scoring subprocess
// invocation. A hung vmaf binary (waiting on a corrupt input pipe, blocked
// on a missing libvmaf model file, deadlocked GPU driver) would otherwise
// hang the entire compare sweep indefinitely. Override via the
// VMAFX_TUNE_SCORE_TIMEOUT environment variable (Go duration string,
// e.g. "5m" or "30s"). 0 or negative values disable the timeout.
const defaultScoreTimeout = 30 * time.Minute

// scoreTimeout returns the per-call vmaf-subprocess timeout, honouring the
// VMAFX_TUNE_SCORE_TIMEOUT environment variable for operator overrides.
func scoreTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_SCORE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return defaultScoreTimeout
}

// DefaultMaxIter is the default maximum number of bisect iterations.
// Typically converges in 5-7 steps for a 64-unit CRF window.
const DefaultMaxIter = 12

// DefaultCRFLo is the default lower bound of the CRF search window.
const DefaultCRFLo = 0

// DefaultCRFHi is the default upper bound of the CRF search window for
// x264/x265. This is the encoder absolute maximum (51), not the perceptual
// informative window.
const DefaultCRFHi = 51

// Sample records one (CRF, bitrate, VMAF) probe from the bisect walk.
// The compare subcommand's JSON output embeds the full list of samples
// so the rate-quality chart can plot them directly (avoids the
// connect-the-dots artefact described in ADR-0530).
type Sample struct {
	CRF          int     `json:"crf"`
	BitratekBps  float64 `json:"bitrate_kbps"`
	VMAFScore    float64 `json:"vmaf_score"`
	EncodeTimeMS float64 `json:"encode_time_ms"`
}

// Result is the outcome of a completed bisect.
type Result struct {
	// BestCRF is the highest CRF (most compression) whose VMAF still meets
	// the target. -1 when no satisfying CRF was found within the search window.
	BestCRF int

	// BestBitratekBps is the bitrate of the encode at BestCRF. 0 when
	// BestCRF == -1.
	BestBitratekBps float64

	// BestVMAFScore is the VMAF score at BestCRF. 0 when BestCRF == -1.
	BestVMAFScore float64

	// BestEncodeTimeMS is the encode wall-time in milliseconds at BestCRF.
	BestEncodeTimeMS float64

	// EncoderVersion is the version string reported by the encoder.
	EncoderVersion string

	// Samples contains every (CRF, bitrate, VMAF) probe the bisect walked
	// through before converging, in iteration order. Used by the rate-quality
	// chart renderer.
	Samples []Sample

	// Iterations is the number of encode+score iterations performed.
	Iterations int
}

// ScoreFunc scores a distorted video file against a reference and returns
// the mean VMAF score. The reference is the original source; the distorted
// is the encoded output produced by the encoder.
//
// Implementations shell out to the vmaf binary or use any other scoring
// mechanism (e.g. a mock for tests).
type ScoreFunc func(ref, distorted string) (float64, error)

// Params configures a bisect run.
type Params struct {
	// TargetVMAF is the minimum VMAF score the encode must achieve.
	TargetVMAF float64

	// CRFLo is the lower bound of the search window (best quality / largest
	// file). Defaults to DefaultCRFLo.
	CRFLo int

	// CRFHi is the upper bound of the search window (lowest quality /
	// smallest file). Defaults to the encoder's CRFRange().max.
	CRFHi int

	// MaxIter caps the bisect iteration count. Defaults to DefaultMaxIter.
	MaxIter int

	// FFmpegBin is the ffmpeg binary path. Defaults to "ffmpeg".
	FFmpegBin string

	// WorkDir is the directory for temporary encode outputs. Defaults to
	// os.TempDir().
	WorkDir string
}

func (p *Params) applyDefaults(enc encoder.Encoder) {
	if p.MaxIter <= 0 {
		p.MaxIter = DefaultMaxIter
	}
	if p.CRFLo == 0 && p.CRFHi == 0 {
		lo, hi := enc.CRFRange()
		p.CRFLo = lo
		p.CRFHi = hi
	}
}

// VMAFScoreFunc returns a ScoreFunc that shells out to the vmaf binary at
// vmafBin. When vmafBin is empty, "vmaf" is looked up on PATH.
//
// The function writes a temporary JSON output file that it removes after
// reading the mean VMAF score.
//
// Each invocation is bounded by scoreTimeout() (default 30 minutes,
// overridable via VMAFX_TUNE_SCORE_TIMEOUT). Without the timeout, a hung
// vmaf child blocks bisect.Run forever; this is especially likely on a
// stuck GPU device, a missing libvmaf model file, or a corrupt input
// pipe — none of which are deadlock-recoverable from the parent without
// an external timeout.
func VMAFScoreFunc(vmafBin string) ScoreFunc {
	if vmafBin == "" {
		vmafBin = "vmaf"
	}
	return func(ref, distorted string) (float64, error) {
		// Write output JSON to a temp file.
		tmp, err := os.CreateTemp("", "vmafx-tune-score-*.json")
		if err != nil {
			return 0, fmt.Errorf("create score temp: %w", err)
		}
		tmpPath := tmp.Name()
		if closeErr := tmp.Close(); closeErr != nil {
			return 0, fmt.Errorf("close score temp: %w", closeErr)
		}
		defer func() {
			if rmErr := os.Remove(tmpPath); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
				slog.Warn("bisect: remove score temp", "error", rmErr, "path", tmpPath)
			}
		}()

		argv := []string{
			vmafBin,
			"--reference", ref,
			"--distorted", distorted,
			"--output", tmpPath,
			"--xml",
		}

		ctx := context.Background()
		var cancel context.CancelFunc = func() {}
		if to := scoreTimeout(); to > 0 {
			ctx, cancel = context.WithTimeout(ctx, to)
		}
		defer cancel()

		// #nosec G204 -- argv[0] is vmafBin (operator-configured at scorerFn
		// construction); argv[1:] mixes fixed flags with `ref` / `distorted`
		// (caller-controlled but bisect is a dev-time tool not an RPC surface)
		// and the os.CreateTemp output path. ctx enforces scoreTimeout().
		cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
		out, runErr := cmd.CombinedOutput()
		if runErr != nil {
			if ctx.Err() == context.DeadlineExceeded {
				return 0, fmt.Errorf("vmaf score timed out after %s: %w\n%s",
					scoreTimeout(), runErr, string(out))
			}
			return 0, fmt.Errorf("vmaf score failed: %w\n%s", runErr, string(out))
		}

		return parseVMAFXMLMean(tmpPath)
	}
}

// parseVMAFXMLMean reads the vmaf XML output and extracts the pooled mean
// VMAF score. Returns an error if the file is missing or malformed.
func parseVMAFXMLMean(xmlPath string) (float64, error) {
	// #nosec G304 -- xmlPath is the os.CreateTemp output passed to vmaf
	// via "--output"; not caller-controlled.
	data, err := os.ReadFile(xmlPath)
	if err != nil {
		return 0, fmt.Errorf("read vmaf output %q: %w", xmlPath, err)
	}
	// The XML contains a line like:
	//   <metric name="vmaf" min="..." max="..." mean="88.123456" harmonic_mean="..."/>
	content := string(data)
	const needle = `name="vmaf"`
	idx := strings.Index(content, needle)
	if idx < 0 {
		return 0, errors.New("vmaf metric not found in XML output")
	}
	// From that position, find mean="..."
	rest := content[idx:]
	const meanPrefix = `mean="`
	mi := strings.Index(rest, meanPrefix)
	if mi < 0 {
		return 0, errors.New("mean attribute not found in vmaf metric element")
	}
	rest = rest[mi+len(meanPrefix):]
	end := strings.IndexByte(rest, '"')
	if end < 0 {
		return 0, errors.New("unterminated mean value in vmaf metric element")
	}
	score, parseErr := strconv.ParseFloat(rest[:end], 64)
	if parseErr != nil {
		return 0, fmt.Errorf("parse vmaf mean %q: %w", rest[:end], parseErr)
	}
	// strconv.ParseFloat accepts the literal tokens "NaN", "+Inf", "-Inf"
	// (and variants) without returning an error. A NaN VMAF score would
	// silently propagate into bisect.Sample.VMAFScore, then into the
	// emitted JSON's bisect_samples, where json.MarshalIndent rejects it
	// with "unsupported value: NaN" and breaks the Python ↔ Go parser-
	// parity invariant (AGENTS.md #2). Reject the corrupt mean attribute
	// at the source so the bisect step records the error rather than a
	// non-finite score.
	if math.IsNaN(score) || math.IsInf(score, 0) {
		return 0, fmt.Errorf("vmaf mean is non-finite (%q); refusing to propagate corrupt score", rest[:end])
	}
	return score, nil
}

// Run performs the bisect and returns the result. It encodes src with enc,
// scoring each attempt with scoreFunc. Temporary encode files are removed
// after each scoring round.
//
// The caller must ensure src is readable for the duration of the bisect.
func Run(
	src string,
	enc encoder.Encoder,
	scoreFunc ScoreFunc,
	params Params,
) (Result, error) {
	params.applyDefaults(enc)

	if params.TargetVMAF <= 0 || params.TargetVMAF > 100 {
		return Result{}, fmt.Errorf("target VMAF %f out of range (0, 100]", params.TargetVMAF)
	}
	if params.CRFLo > params.CRFHi {
		return Result{}, fmt.Errorf("CRFLo %d > CRFHi %d", params.CRFLo, params.CRFHi)
	}

	lo := params.CRFLo
	hi := params.CRFHi

	var (
		bestCRF          = -1
		bestBitrate      float64
		bestVMAF         float64
		bestEncodeTimeMS float64
		bestVersion      string
		samples          []Sample
		iterations       int
	)

	for lo <= hi && iterations < params.MaxIter {
		// Round the midpoint toward hi (lower quality / higher CRF) so we
		// never accept a CRF we haven't measured.
		mid := (lo + hi + 1) / 2
		iterations++

		ep := encoder.EncodeParams{
			CRF:       mid,
			FFmpegBin: params.FFmpegBin,
			OutputDir: params.WorkDir,
		}
		encResult, encErr := enc.Encode(src, ep)
		if encErr != nil {
			return Result{}, fmt.Errorf("encode at CRF %d: %w", mid, encErr)
		}
		encodedPath := encResult.OutputPath

		vmafScore, scoreErr := scoreFunc(src, encodedPath)
		// Always clean up the encoded file regardless of scoring outcome.
		// If both score and remove fail, surface both via errors.Join so the
		// caller can see the disk-leak alongside the scoring failure.
		removeErr := os.Remove(encodedPath)
		if scoreErr != nil || removeErr != nil {
			var joinErrs []error
			if scoreErr != nil {
				joinErrs = append(joinErrs, fmt.Errorf("score at CRF %d: %w", mid, scoreErr))
			}
			if removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
				joinErrs = append(joinErrs, fmt.Errorf("remove encoded temp %q at CRF %d: %w", encodedPath, mid, removeErr))
			}
			if len(joinErrs) > 0 {
				return Result{}, errors.Join(joinErrs...)
			}
		}

		samples = append(samples, Sample{
			CRF:          mid,
			BitratekBps:  encResult.BitratekBps,
			VMAFScore:    vmafScore,
			EncodeTimeMS: encResult.EncodeTimeMS,
		})

		if vmafScore >= params.TargetVMAF {
			// This CRF meets the target. Record as best so far and try a
			// higher CRF (more compression).
			bestCRF = mid
			bestBitrate = encResult.BitratekBps
			bestVMAF = vmafScore
			bestEncodeTimeMS = encResult.EncodeTimeMS
			bestVersion = encResult.EncoderVersion
			lo = mid + 1
		} else {
			// This CRF does not meet the target. Need lower CRF (more
			// quality / larger file).
			hi = mid - 1
		}
	}

	return Result{
		BestCRF:          bestCRF,
		BestBitratekBps:  bestBitrate,
		BestVMAFScore:    bestVMAF,
		BestEncodeTimeMS: bestEncodeTimeMS,
		EncoderVersion:   bestVersion,
		Samples:          samples,
		Iterations:       iterations,
	}, nil
}

// IterSamples returns a single-shot iterator over the bisect probes that
// produced this Result, in iteration order.  Equivalent to ranging over
// r.Samples but adapter-friendly for callers that stream the bisect walk
// into a gRPC response, an event log, or a chart-rendering pipeline
// without materialising the slice into their own working set.
//
// The Samples field remains the authoritative store and is what the JSON
// schema serialises; IterSamples is an ergonomic adapter, not a replacement.
// Mutating r.Samples after the iterator is created has the usual slice-range
// semantics — the iterator captures the slice header at call time, not at
// each yield.
//
// Added in v3.x.y-lusoris.N+1 (ADR-0932).  Callers iterating linearly with
// no random access or re-iteration are encouraged to prefer this method.
func (r Result) IterSamples() iter.Seq[Sample] {
	return func(yield func(Sample) bool) {
		for _, s := range r.Samples {
			if !yield(s) {
				return
			}
		}
	}
}
