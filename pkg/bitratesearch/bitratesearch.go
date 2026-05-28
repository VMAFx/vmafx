// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package bitratesearch implements a bitrate-domain binary search to find the
// minimum bitrate that achieves a target VMAF score for a fixed encoder.
//
// Motivation (ADR-0734 §bisect subcommand):
//
// The CRF-domain bisect in pkg/bisect finds the highest CRF (lowest quality
// knob) that meets a VMAF target.  The resulting bitrate is an indirect
// consequence of CRF + content complexity — it is not a monotone function of
// CRF, so there is no guarantee that the CRF bisect produces the minimum
// bitrate that meets the target.
//
// The bitrate-domain bisect inverts the relationship: for a fixed bitrate the
// encoder is run in VBR mode (--video-bitrate / -b:v) and the resulting VMAF
// is measured.  The binary search converges to the lowest bitrate that meets
// the target within a caller-supplied tolerance.
//
// Algorithm:
//  1. Probe lo = BitrateMinKbps: if VMAF ≥ target the answer is lo.
//  2. Probe hi = BitrateMaxKbps: if VMAF < target, no bitrate in the window
//     meets the target (return BestBitrateKbps = -1).
//  3. Otherwise bisect mid = (lo+hi)/2; keep the lower half when VMAF ≥
//     target, upper half when VMAF < target.
//  4. Stop when hi − lo ≤ Tolerance kbps or MaxIter is reached.
//
// The output JSON schema mirrors the Python vmaf-tune bisect subcommand output
// (field names identical; Go adds an explicit schema_version field per the
// ADR-0705 schema-forward invariant).
package bitratesearch

import (
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"strconv"
	"time"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/encoder"
)

// DefaultMaxIter is the default maximum number of bitrate-bisect iterations.
// A 10000-500 kbps window converges in ceil(log2(9500/0.5)) ≈ 15 iterations.
const DefaultMaxIter = 20

// DefaultTolerance is the default convergence tolerance in kbps.
const DefaultTolerance = 50.0

// Sample records one (bitrate, vmaf) probe from the bitrate bisect walk.
type Sample struct {
	BitrateKbps  float64 `json:"bitrate_kbps"`
	VMAFScore    float64 `json:"vmaf_score"`
	EncodeTimeMS float64 `json:"encode_time_ms"`
}

// Result is the outcome of a completed bitrate bisect.
type Result struct {
	// BestBitrateKbps is the lowest bitrate (kbps) that achieves the target
	// VMAF.  -1 when no bitrate in the search window achieves the target.
	BestBitrateKbps float64 `json:"best_bitrate_kbps"`

	// BestVMAFScore is the VMAF score at BestBitrateKbps.  0 when -1.
	BestVMAFScore float64 `json:"best_vmaf_score"`

	// BestEncodeTimeMS is the encode wall-time in milliseconds at the best bitrate.
	BestEncodeTimeMS float64 `json:"best_encode_time_ms"`

	// Converged is true when the bisect stopped because hi−lo ≤ Tolerance,
	// false when it stopped because MaxIter was reached.
	Converged bool `json:"converged"`

	// Iterations is the number of encode+score iterations performed.
	Iterations int `json:"iterations"`

	// Samples contains every (bitrate, vmaf) probe in iteration order.
	Samples []Sample `json:"samples"`
}

// Params configures a bitrate bisect run.
type Params struct {
	// TargetVMAF is the minimum acceptable VMAF score.
	TargetVMAF float64

	// BitrateMinKbps is the lower bound of the bitrate search window.
	// Defaults to 500 kbps when ≤ 0.
	BitrateMinKbps float64

	// BitrateMaxKbps is the upper bound of the bitrate search window.
	// Defaults to 10000 kbps when ≤ 0.
	BitrateMaxKbps float64

	// Tolerance is the convergence threshold in kbps.  The bisect stops when
	// hi − lo ≤ Tolerance.  Defaults to DefaultTolerance.
	Tolerance float64

	// MaxIter caps the bisect iteration count.  Defaults to DefaultMaxIter.
	MaxIter int

	// FFmpegBin is the ffmpeg binary path.  Defaults to "ffmpeg".
	FFmpegBin string

	// WorkDir is the directory for temporary encode outputs.  Defaults to
	// os.TempDir().
	WorkDir string

	// ScaleWidth and ScaleHeight apply resolution-aware downscaling before
	// each encode (ADR-0734).  0 = no scaling.
	ScaleWidth  int
	ScaleHeight int
}

func (p *Params) applyDefaults() {
	if p.BitrateMinKbps <= 0 {
		p.BitrateMinKbps = 500
	}
	if p.BitrateMaxKbps <= 0 {
		p.BitrateMaxKbps = 10000
	}
	if p.Tolerance <= 0 {
		p.Tolerance = DefaultTolerance
	}
	if p.MaxIter <= 0 {
		p.MaxIter = DefaultMaxIter
	}
	if p.FFmpegBin == "" {
		p.FFmpegBin = "ffmpeg"
	}
	if p.WorkDir == "" {
		p.WorkDir = os.TempDir()
	}
}

// Run performs the bitrate-domain bisect and returns the result.  It encodes
// src with enc in VBR mode at each probed bitrate, scoring each attempt with
// scoreFunc.  Temporary encode files are removed after each scoring round.
func Run(
	src string,
	enc encoder.Encoder,
	scoreFunc bisect.ScoreFunc,
	params Params,
) (Result, error) {
	params.applyDefaults()

	if params.TargetVMAF <= 0 || params.TargetVMAF > 100 {
		return Result{}, fmt.Errorf("target VMAF %f out of range (0, 100]", params.TargetVMAF)
	}
	if params.BitrateMinKbps >= params.BitrateMaxKbps {
		return Result{}, fmt.Errorf("BitrateMinKbps (%g) ≥ BitrateMaxKbps (%g)",
			params.BitrateMinKbps, params.BitrateMaxKbps)
	}

	// Determine ffprobe path from ffmpeg path.
	ffmpegBin := params.FFmpegBin

	var (
		bestBitrateKbps  = -1.0
		bestVMAF         float64
		bestEncodeTimeMS float64
		samples          []Sample
		iterations       int
	)

	lo := params.BitrateMinKbps
	hi := params.BitrateMaxKbps

	probe := func(targetBitrateKbps float64) (vmafScore float64, encTimeMS float64, err error) {
		iterations++
		encResult, encErr := encodeAtBitrate(src, enc, targetBitrateKbps, ffmpegBin, params)
		if encErr != nil {
			return 0, 0, fmt.Errorf("encode at %.0f kbps: %w", targetBitrateKbps, encErr)
		}
		vmaf, scoreErr := scoreFunc(src, encResult.OutputPath)
		_ = os.Remove(encResult.OutputPath)
		if scoreErr != nil {
			return 0, 0, fmt.Errorf("score at %.0f kbps: %w", targetBitrateKbps, scoreErr)
		}
		samples = append(samples, Sample{
			BitrateKbps:  targetBitrateKbps,
			VMAFScore:    vmaf,
			EncodeTimeMS: encResult.EncodeTimeMS,
		})
		return vmaf, encResult.EncodeTimeMS, nil
	}

	// Phase 1: probe the lower bound to see if the target is already met.
	loVMAF, loTimeMS, loErr := probe(lo)
	if loErr != nil {
		return Result{}, loErr
	}
	if loVMAF >= params.TargetVMAF {
		return Result{
			BestBitrateKbps:  lo,
			BestVMAFScore:    loVMAF,
			BestEncodeTimeMS: loTimeMS,
			Converged:        true,
			Iterations:       iterations,
			Samples:          samples,
		}, nil
	}

	// Phase 2: probe the upper bound to confirm the window is viable.
	hiVMAF, hiTimeMS, hiErr := probe(hi)
	if hiErr != nil {
		return Result{}, hiErr
	}
	if hiVMAF >= params.TargetVMAF {
		bestBitrateKbps = hi
		bestVMAF = hiVMAF
		bestEncodeTimeMS = hiTimeMS
	} else {
		// Upper bound does not meet target — report not-found.
		return Result{
			BestBitrateKbps:  -1,
			Converged:        false,
			Iterations:       iterations,
			Samples:          samples,
		}, nil
	}

	// Phase 3: binary search.
	converged := false
	for iterations < params.MaxIter {
		if hi-lo <= params.Tolerance {
			converged = true
			break
		}
		mid := (lo + hi) / 2
		midVMAF, midTimeMS, midErr := probe(mid)
		if midErr != nil {
			return Result{}, midErr
		}
		if midVMAF >= params.TargetVMAF {
			// mid meets target — record as best and search lower half.
			bestBitrateKbps = mid
			bestVMAF = midVMAF
			bestEncodeTimeMS = midTimeMS
			hi = mid
		} else {
			// mid does not meet target — search upper half.
			lo = mid
		}
	}
	if hi-lo <= params.Tolerance {
		converged = true
	}

	return Result{
		BestBitrateKbps:  bestBitrateKbps,
		BestVMAFScore:    bestVMAF,
		BestEncodeTimeMS: bestEncodeTimeMS,
		Converged:        converged,
		Iterations:       iterations,
		Samples:          samples,
	}, nil
}

// encodeResult is the minimal EncodeResult used internally by encodeAtBitrate.
type encodeResult struct {
	OutputPath   string
	EncodeTimeMS float64
}

// encodeAtBitrate encodes src at targetBitrateKbps using VBR mode.  It shells
// out to ffmpeg using the encoder's codec name with -b:v (target bitrate)
// instead of the CRF flag.
func encodeAtBitrate(
	src string,
	enc encoder.Encoder,
	targetBitrateKbps float64,
	ffmpegBin string,
	params Params,
) (encodeResult, error) {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	dir := params.WorkDir
	if dir == "" {
		dir = os.TempDir()
	}

	tmpFile, err := os.CreateTemp(dir,
		fmt.Sprintf("vmafx-tune-bitratesearch-%s-%.0fkbps-*.mkv", enc.Name(), targetBitrateKbps))
	if err != nil {
		return encodeResult{}, fmt.Errorf("create temp output: %w", err)
	}
	outPath := tmpFile.Name()
	if closeErr := tmpFile.Close(); closeErr != nil {
		return encodeResult{}, fmt.Errorf("close temp file: %w", closeErr)
	}
	if removeErr := os.Remove(outPath); removeErr != nil {
		return encodeResult{}, fmt.Errorf("remove temp placeholder: %w", removeErr)
	}

	// Convert kbps to bps string.
	bitrateStr := strconv.FormatInt(int64(math.Round(targetBitrateKbps*1000)), 10)

	argv := []string{
		ffmpegBin,
		"-hide_banner",
		"-loglevel", "warning",
		"-y",
		"-i", src,
		"-an",
	}
	if params.ScaleWidth > 0 && params.ScaleHeight > 0 {
		argv = append(argv,
			"-vf", fmt.Sprintf("scale=%d:%d:flags=lanczos", params.ScaleWidth, params.ScaleHeight))
	}
	argv = append(argv,
		"-c:v", enc.Name(),
		"-b:v", bitrateStr,
		outPath,
	)

	t0 := time.Now()
	cmd := exec.Command(argv[0], argv[1:]...) //nolint:gosec -- argv is constructed from validated inputs
	out, runErr := cmd.CombinedOutput()
	elapsed := time.Since(t0)

	if runErr != nil {
		_ = os.Remove(outPath)
		return encodeResult{}, fmt.Errorf("ffmpeg vbr encode failed (%.0f kbps): %w\n%s",
			targetBitrateKbps, runErr, string(out))
	}
	return encodeResult{
		OutputPath:   outPath,
		EncodeTimeMS: float64(elapsed.Milliseconds()),
	}, nil
}

// ErrNoConvergence is returned by WrapResult when the bisect did not find a
// bitrate meeting the target.
var ErrNoConvergence = errors.New("no bitrate in search window achieves target VMAF")
