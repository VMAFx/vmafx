// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/corpus/corpus.go — Phase A corpus orchestrator (Go port of
// vmaftune.corpus).
//
// Sweeps a (preset, crf) grid against one or more references, runs the
// encoder, scores each encode against the reference with the libvmaf CLI, and
// yields one JSONL row per (source, preset, crf) combination.
//
// The row schema lives in schema.go (RowKeys, SchemaVersion); Phase B/C are
// downstream consumers, so bumping it is a coordinated change.

package corpus

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/cliopt"
	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/pyjson"
)

// Job is one source plus the list of (preset, crf) cells to evaluate.
//
// Width / Height are the *rung target* (encode output and libvmaf score)
// dimensions. SrcWidth / SrcHeight (ADR-0498) are optional source-side
// overrides used when the raw-YUV source's actual geometry differs from the
// rung target: the encode pipe then tells ffmpeg the true source -s W:H and a
// -vf scale=W:H filter downscales to the rung target. Zero for both keeps the
// legacy single-resolution behaviour.
type Job struct {
	Source    string
	Width     int
	Height    int
	PixFmt    string
	Framerate float64
	DurationS float64
	Cells     []Cell
	SrcWidth  int
	SrcHeight int
}

// HDR modes accepted by Options.HDRMode (ADR-0300).
const (
	HDRModeAuto     = "auto"
	HDRModeForceSDR = "force-sdr"
	HDRModeForcePQ  = "force-hdr-pq"
	HDRModeForceHLG = "force-hdr-hlg"
)

var validHDRModes = map[string]bool{
	HDRModeAuto:     true,
	HDRModeForceSDR: true,
	HDRModeForcePQ:  true,
	HDRModeForceHLG: true,
}

// Options are the knobs that govern a corpus run.
type Options struct {
	Encoder    string
	Output     string
	EncodeDir  string
	VMAFModel  string
	FFmpegBin  string
	VMAFBin    string
	FFprobeBin string

	// KeepEncodes retains encoded outputs after scoring.
	KeepEncodes bool
	// SrcSHA256 computes the source hash for row provenance.
	SrcSHA256 bool

	// SampleClipSeconds opts the run into sample-clip mode (ADR-0297):
	// each grid point encodes the centre N-second window of the reference
	// and scores the matching reference window via the libvmaf CLI's
	// --frame_skip_ref / --frame_cnt. 0.0 keeps the full-source behaviour.
	// Bitrate and timing are reported as measured on the slice, so Phase
	// B/C consumers should weight or filter on clip_mode rather than mixing
	// sample and full rows blindly.
	SampleClipSeconds float64

	// ScoreBackend is the resolved libvmaf backend. "" omits the --backend
	// flag so libvmaf picks its own default. The CLI resolves
	// "--score-backend auto" to a concrete value before populating this;
	// Options itself never walks the fallback chain.
	ScoreBackend string

	// HDRMode is one of the HDRMode* constants (ADR-0300).
	HDRMode string

	// TwoPass opts into 2-pass encoding for adapters that support it
	// (ADR-0333). Adapters where SupportsTwoPass is false emit a one-line
	// stderr warning and run single-pass.
	TwoPass bool

	// ResolutionAware overrides VMAFModel with the resolution-appropriate
	// model returned by SelectVMAFModelVersion (ADR-0289).
	ResolutionAware bool
}

// NewOptions returns Options populated with the Python dataclass defaults.
func NewOptions() Options {
	return Options{
		Encoder:         "libx264",
		Output:          "corpus.jsonl",
		EncodeDir:       filepath.Join(".workingdir2", "encodes"),
		VMAFModel:       Model1080P,
		FFmpegBin:       "ffmpeg",
		VMAFBin:         "vmaf",
		FFprobeBin:      "ffprobe",
		SrcSHA256:       true,
		HDRMode:         HDRModeAuto,
		ResolutionAware: true,
	}
}

// Runners bundles the subprocess seams IterRows drives. A nil field falls back
// to ExecRunner.
//
// Encode / Score mirror the Python encode_runner / score_runner stubs; Shot and
// Probe cover vmaf-perShot and ffprobe. Decode is deliberately separate: the
// Python pipeline always uses the real subprocess for its ffmpeg decode steps
// because test stubs injected via score_runner mock the vmaf CLI, not ffmpeg.
type Runners struct {
	Encode Runner
	Score  Runner
	Shot   Runner
	Probe  Runner
	Decode Runner
}

// encodePath is the deterministic per-cell encode destination.
func encodePath(opts Options, source, preset string, crf int) string {
	stem := strings.TrimSuffix(filepath.Base(source), filepath.Ext(source))
	name := fmt.Sprintf("%s__%s__%s__crf%d.mp4", stem, opts.Encoder, preset, crf)
	return filepath.Join(opts.EncodeDir, name)
}

// sampleClipPlan is the resolved sample-clip window for one job.
type sampleClipPlan struct {
	ClipSeconds  float64
	StartS       float64
	FrameSkipRef int
	FrameCnt     int
	ClipMode     string
}

// resolveSampleClip caps the requested slice at the job duration, so a
// 10-second request against an 8-second source falls back to full-clip rather
// than encoding a short tail. The window is centre-anchored.
func resolveSampleClip(job Job, opts Options) sampleClipPlan {
	requested := opts.SampleClipSeconds
	duration := job.DurationS
	if requested <= 0.0 || duration <= 0.0 || requested >= duration {
		return sampleClipPlan{ClipMode: "full"}
	}
	startS := math.Max(0.0, (duration-requested)/2.0)
	// The libvmaf CLI takes integer frame counts and the framerate may be
	// fractional (23.976); round to nearest to keep the window symmetric
	// around the centre.
	return sampleClipPlan{
		ClipSeconds:  requested,
		StartS:       startS,
		FrameSkipRef: int(math.Round(startS * job.Framerate)),
		FrameCnt:     int(math.Round(requested * job.Framerate)),
		ClipMode:     fmt.Sprintf("sample_%ds", int(math.Round(requested))),
	}
}

// syntheticHDRInfo builds an HdrInfo for --force-hdr-pq / --force-hdr-hlg.
//
// Used when the user knows the source carries the named transfer but the
// container cannot surface it (raw YUV references are the canonical case).
// Mastering-display and max-CLL stay empty — without ffprobe there is no way to
// read them, and emitting fabricated values would be worse than emitting none.
func syntheticHDRInfo(transfer, pixFmt string) *HdrInfo {
	forcedPixFmt := pixFmt
	if !strings.Contains(pixFmt, "10") && !strings.Contains(pixFmt, "12") {
		forcedPixFmt = "yuv420p10le"
	}
	return &HdrInfo{
		Transfer:   transfer,
		Primaries:  "bt2020",
		Matrix:     "bt2020nc",
		ColorRange: "tv",
		PixFmt:     forcedPixFmt,
	}
}

// resolveHDR resolves the effective HDR signalling for job per opts.HDRMode.
//
// It returns (info, forced) where info is nil for SDR and forced records
// whether the user overrode auto-detection via a --force-* flag. The flag lands
// on the row's hdr_forced column so Phase B/C consumers can distinguish a
// detected HDR row from a user-asserted one.
//
// An unknown HDRMode falls back to "auto" with a warning — corpus runs should
// not crash on a typoed CLI flag.
func resolveHDR(ctx context.Context, job Job, opts Options, run Runner) (*HdrInfo, bool) {
	mode := opts.HDRMode
	if !validHDRModes[mode] {
		slog.Warn("vmaf-tune: unknown hdr_mode; falling back to 'auto'", "hdr_mode", mode)
		mode = HDRModeAuto
	}
	switch mode {
	case HDRModeForceSDR:
		return nil, true
	case HDRModeForcePQ:
		return syntheticHDRInfo("pq", job.PixFmt), true
	case HDRModeForceHLG:
		return syntheticHDRInfo("hlg", job.PixFmt), true
	}
	// auto: probe the source. DetectHDR returns nil for SDR, probe failure,
	// and a missing binary — all fine; the encode proceeds without HDR
	// signalling.
	return DetectHDR(ctx, job.Source, opts.FFprobeBin, run), false
}

// resolveHDRScoreModel picks the VMAF model to score against given HDR
// provenance.
//
// nil info uses the SDR model unchanged. When info is set we look for an HDR
// model JSON under model/; if none is present we warn once per corpus run and
// fall back to the SDR model — the HDR-VMAF model port is a backlog item
// (ADR-0300). warned is a one-element flag so the warning fires exactly once
// per IterRows invocation rather than per cell.
func resolveHDRScoreModel(info *HdrInfo, sdrModel string, warned *bool) string {
	if info == nil {
		return sdrModel
	}
	if hdrModel := SelectHDRVMAFModel("", info.Transfer); hdrModel != "" {
		// ADR-1190: escape ":"/"="/"\\" so the CLI parser reads the path back whole.
		return "path=" + cliopt.EscapeValue(hdrModel)
	}
	if !*warned {
		slog.Warn("vmaf-tune: HDR source detected but no HDR VMAF model is shipped; "+
			"scoring against the SDR model — scores will trend low for high-luminance "+
			"regions (see ADR-0300)",
			"transfer", info.Transfer, "sdr_model", sdrModel)
		*warned = true
	}
	return sdrModel
}

// resolveShotMetadata runs shot detection once per source and aggregates it.
//
// Failures are silent: DetectShotsWithStatus already falls back to a sentinel
// list when the binary is missing or the invocation fails, and SummariseShots
// maps that sentinel to the all-zero metadata downstream consumers read as
// "shot data unavailable". The cost is paid once per source, not per cell.
func resolveShotMetadata(ctx context.Context, job Job, run Runner) ShotMetadata {
	totalFrames := 0
	if job.DurationS > 0.0 && job.Framerate > 0.0 {
		totalFrames = int(math.Round(job.DurationS * job.Framerate))
	}
	shots, ok := DetectShotsWithStatus(ctx, job.Source, DetectShotsOptions{
		Width:       job.Width,
		Height:      job.Height,
		PixFmt:      job.PixFmt,
		TotalFrames: totalFrames,
		PerShotBin:  "vmaf-perShot",
	}, run)
	if !ok {
		return ShotMetadata{}
	}
	return SummariseShots(shots, job.Framerate)
}

// decodeSourceParams carries the optional geometry hints decodeSourceToYUV
// needs for a raw-YUV demuxer block or a cross-resolution scale filter.
type decodeSourceParams struct {
	PixFmt          string
	DurationS       float64
	FFmpegBin       string
	TargetWidth     int
	TargetHeight    int
	SourceWidth     int
	SourceHeight    int
	SourceFramerate float64
	SourceIsRaw     bool
}

// decodeSourceToYUV runs "ffmpeg -i source -f rawvideo -pix_fmt P destination".
//
// Shared building block for the reference and distorted decode paths. When
// TargetWidth and TargetHeight are both set, a "-vf scale=W:H" lands the
// decoded raw YUV at the requested rendition geometry rather than the source's
// native geometry (ADR-0501 Bug #V4-B) — without it the libvmaf CLI mis-parses
// a 1920x1080 reference as a 1280x720 frame and produces nonsense VMAF (~21
// instead of ~93).
//
// When SourceIsRaw is set, the demuxer-side "-f rawvideo -pix_fmt … -s WxH -r
// FR" block is inserted BEFORE -i so ffmpeg can parse the raw YUV at all
// (ADR-0506 Bug #V6-2).
func decodeSourceToYUV(
	ctx context.Context, source, destination string, p decodeSourceParams, run Runner,
) (int, error) {
	if err := os.MkdirAll(filepath.Dir(destination), 0o750); err != nil {
		return 1, fmt.Errorf("create decode dir: %w", err)
	}
	cmd := []string{ffmpegBinOrDefault(p.FFmpegBin), "-y", "-hide_banner", "-loglevel", "error"}
	if p.SourceIsRaw {
		if p.SourceWidth <= 0 || p.SourceHeight <= 0 {
			return 1, fmt.Errorf(
				"decodeSourceToYUV: SourceIsRaw requires SourceWidth and SourceHeight")
		}
		framerate := p.SourceFramerate
		if framerate == 0 {
			framerate = 24.0
		}
		cmd = append(cmd,
			"-f", "rawvideo",
			"-pix_fmt", p.PixFmt,
			"-s", fmt.Sprintf("%dx%d", p.SourceWidth, p.SourceHeight),
			"-r", pyjson.FloatRepr(framerate),
		)
	}
	cmd = append(cmd, "-i", source)
	cmd = append(cmd, "-f", "rawvideo", "-pix_fmt", p.PixFmt)
	if p.TargetWidth > 0 && p.TargetHeight > 0 {
		cmd = append(cmd, "-vf", fmt.Sprintf("scale=%d:%d", p.TargetWidth, p.TargetHeight))
	}
	if p.DurationS > 0.0 {
		cmd = append(cmd, "-t", pyjson.FloatRepr(p.DurationS))
	}
	cmd = append(cmd, destination)
	return runnerOrExec(run)(ctx, cmd).ReturnCode, nil
}

// maybeDecodeReference decodes a container reference to raw YUV once per
// IterRows call.
//
// It returns (referencePath, returncode). rc == 0 with the original source when
// the source is already raw and no rescale is needed; a non-zero rc with the
// original source signals a decode failure, and callers should treat every
// (preset, crf) cell as failed rather than invoking the vmaf binary on an
// undecodable file.
func maybeDecodeReference(
	ctx context.Context, source, encodeDir string, p decodeSourceParams, run Runner,
) (string, int) {
	sourceIsRaw := IsRawYUVPath(source)
	p.SourceIsRaw = sourceIsRaw
	if sourceIsRaw && p.TargetWidth == 0 && p.TargetHeight == 0 {
		return source, 0
	}
	stem := strings.TrimSuffix(filepath.Base(source), filepath.Ext(source))
	var decoded string
	if p.TargetWidth > 0 && p.TargetHeight > 0 {
		// Per-rung sidecar — embed WxH in the filename so multi-rung
		// sweeps do not collide on a stale decode (ADR-0501).
		decoded = filepath.Join(encodeDir,
			fmt.Sprintf("%s.ref.decoded.%dx%d.yuv", stem, p.TargetWidth, p.TargetHeight))
	} else {
		decoded = filepath.Join(encodeDir, stem+".ref.decoded.yuv")
	}
	// Re-use a previous decode: the same source and window length is
	// constant across every cell, so one decode suffices.
	if _, err := os.Stat(decoded); err == nil {
		return decoded, 0
	}
	rc, err := decodeSourceToYUV(ctx, source, decoded, p, run)
	if err == nil && rc == 0 {
		if _, statErr := os.Stat(decoded); statErr == nil {
			return decoded, 0
		}
	}
	slog.Warn("corpus: ffmpeg decode of reference failed; every (preset, crf) cell "+
		"will record exit_status != 0", "source", source, "rc", rc)
	if rc == 0 {
		rc = 1
	}
	return source, rc
}

type iterationState struct {
	adapter           *codecadapter.Adapter
	srcHash           string
	clip              sampleClipPlan
	shotMeta          ShotMetadata
	hdrInfo           *HdrInfo
	hdrForced         bool
	hdrExtraParams    []string
	baseScoreModel    string
	scoreModelWarned  bool
	decodedReference  string
	referenceDecodeRC int
}

// IterRows produces and streams one JSONL row per (preset, crf) cell.
func IterRows(
	ctx context.Context, job Job, opts Options, runners Runners,
	emit func(map[string]any) error,
) error {
	state, err := prepareIterationState(ctx, job, opts, runners)
	if err != nil {
		return err
	}
	for _, cell := range job.Cells {
		if cellErr := emitIterationCell(ctx, job, opts, runners, state, cell, emit); cellErr != nil {
			return cellErr
		}
	}
	return nil
}

func prepareIterationState(
	ctx context.Context, job Job, opts Options, runners Runners,
) (*iterationState, error) {
	adapter, err := codecadapter.Get(opts.Encoder)
	if err != nil {
		return nil, err
	}
	srcHash, err := iterationSourceHash(job.Source, opts.SrcSHA256)
	if err != nil {
		return nil, err
	}
	if err = os.MkdirAll(opts.EncodeDir, 0o750); err != nil {
		return nil, fmt.Errorf("create encode dir: %w", err)
	}
	baseModel, err := iterationScoreModel(job, opts)
	if err != nil {
		return nil, err
	}
	hdrInfo, hdrForced := resolveHDR(ctx, job, opts, runners.Probe)
	state := &iterationState{
		adapter: adapter, srcHash: srcHash, clip: resolveSampleClip(job, opts),
		shotMeta: resolveShotMetadata(ctx, job, runners.Shot),
		hdrInfo:  hdrInfo, hdrForced: hdrForced, baseScoreModel: baseModel,
	}
	if hdrInfo != nil {
		state.hdrExtraParams = HDRCodecArgs(opts.Encoder, hdrInfo)
	}
	state.decodedReference, state.referenceDecodeRC = maybeDecodeReference(
		ctx, job.Source, opts.EncodeDir, referenceDecodeParams(job, opts), runners.Decode)
	return state, nil
}

func iterationSourceHash(source string, enabled bool) (string, error) {
	if !enabled {
		return "", nil
	}
	hash, err := FileSHA256(source)
	if err != nil {
		return "", fmt.Errorf("hash source %q: %w", source, err)
	}
	return hash, nil
}

func iterationScoreModel(job Job, opts Options) (string, error) {
	if !opts.ResolutionAware {
		return opts.VMAFModel, nil
	}
	model, err := SelectVMAFModelVersion(job.Width, job.Height)
	if err != nil {
		return "", fmt.Errorf("select VMAF model: %w", err)
	}
	return model, nil
}

func referenceDecodeParams(job Job, opts Options) decodeSourceParams {
	targetWidth, targetHeight := 0, 0
	if job.SrcWidth > 0 && job.SrcHeight > 0 &&
		(job.SrcWidth != job.Width || job.SrcHeight != job.Height) {
		targetWidth, targetHeight = job.Width, job.Height
	}
	sourceWidth, sourceHeight := job.Width, job.Height
	if job.SrcWidth > 0 {
		sourceWidth = job.SrcWidth
	}
	if job.SrcHeight > 0 {
		sourceHeight = job.SrcHeight
	}
	return decodeSourceParams{
		PixFmt: job.PixFmt, DurationS: job.DurationS, FFmpegBin: opts.FFmpegBin,
		TargetWidth: targetWidth, TargetHeight: targetHeight,
		SourceWidth: sourceWidth, SourceHeight: sourceHeight,
		SourceFramerate: job.Framerate,
	}
}

func emitIterationCell(
	ctx context.Context, job Job, opts Options, runners Runners,
	state *iterationState, cell Cell, emit func(map[string]any) error,
) error {
	if err := state.adapter.Validate(cell.Preset, cell.CRF); err != nil {
		return err
	}
	out := encodePath(opts, job.Source, cell.Preset, cell.CRF)
	scoreModel := resolveHDRScoreModel(
		state.hdrInfo, state.baseScoreModel, &state.scoreModelWarned)
	if state.referenceDecodeRC != 0 {
		return emit(referenceDecodeFailureRow(job, opts, state, cell, out, scoreModel))
	}
	encReq := buildEncodeRequest(
		job, opts, state.adapter, cell, out, state.clip, state.hdrExtraParams)
	encRes := runCellEncode(ctx, opts, runners, state.adapter, encReq)
	scoreReq := cellScoreRequest(job, state, out, scoreModel)
	scoreRes := runCellScore(ctx, opts, runners, encRes, scoreReq)
	row := buildRow(rowInput{
		Job: job, Opts: opts, Cell: cell, SrcSHA: state.srcHash,
		Enc: encRes, Score: scoreRes, ScoreModel: scoreModel,
		ClipMode: state.clip.ClipMode, HDRInfo: state.hdrInfo,
		HDRForced: state.hdrForced, ShotMeta: state.shotMeta,
	})
	if err := cleanupCellEncode(opts, encRes, out); err != nil {
		return err
	}
	return emit(row)
}

func referenceDecodeFailureRow(
	job Job, opts Options, state *iterationState, cell Cell, out, scoreModel string,
) map[string]any {
	rc := state.referenceDecodeRC
	enc := EncodeResult{
		Request: EncodeRequest{
			Source: job.Source, Width: job.Width, Height: job.Height,
			PixFmt: job.PixFmt, Framerate: job.Framerate,
			Encoder: state.adapter.Encoder, Preset: cell.Preset,
			CRF: cell.CRF, Output: out,
		},
		EncoderVersion: "skipped", FFmpegVersion: "skipped", ExitStatus: rc,
		StderrTail: fmt.Sprintf("encode skipped: reference decode failed (rc=%d)", rc),
	}
	score := ScoreResult{
		Request: ScoreRequest{
			Reference: state.decodedReference, Distorted: out,
			Width: job.Width, Height: job.Height, PixFmt: job.PixFmt,
			Model: scoreModel, FrameSkipRef: state.clip.FrameSkipRef,
			FrameCnt: state.clip.FrameCnt, DurationS: job.DurationS,
		},
		VMAFScore: math.NaN(), VMAFBinaryVersion: "skipped", ExitStatus: rc,
		StderrTail: fmt.Sprintf(
			"reference decode to raw YUV failed (rc=%d) for %s", rc, job.Source),
	}
	return buildRow(rowInput{
		Job: job, Opts: opts, Cell: cell, SrcSHA: state.srcHash,
		Enc: enc, Score: score, ScoreModel: scoreModel, ClipMode: state.clip.ClipMode,
		HDRInfo: state.hdrInfo, HDRForced: state.hdrForced, ShotMeta: state.shotMeta,
	})
}

func runCellEncode(
	ctx context.Context, opts Options, runners Runners,
	adapter *codecadapter.Adapter, request EncodeRequest,
) EncodeResult {
	switch {
	case opts.TwoPass:
		return RunTwoPassEncode(ctx, request, opts.FFmpegBin, runners.Encode, "")
	case adapter.SupportsEncoderStats:
		return RunEncodeWithStats(ctx, request, opts.FFmpegBin, runners.Encode, "")
	default:
		return RunEncode(ctx, request, opts.FFmpegBin, runners.Encode)
	}
}

func cellScoreRequest(
	job Job, state *iterationState, out, scoreModel string,
) ScoreRequest {
	return ScoreRequest{
		Reference: state.decodedReference, Distorted: out,
		Width: job.Width, Height: job.Height, PixFmt: job.PixFmt,
		Model: scoreModel, FrameSkipRef: state.clip.FrameSkipRef,
		FrameCnt: state.clip.FrameCnt, DurationS: job.DurationS,
	}
}

func runCellScore(
	ctx context.Context, opts Options, runners Runners,
	encRes EncodeResult, request ScoreRequest,
) ScoreResult {
	if encRes.ExitStatus != 0 {
		return ScoreResult{
			Request: request, VMAFScore: math.NaN(), VMAFBinaryVersion: "skipped",
			ExitStatus: encRes.ExitStatus, StderrTail: "encode failed; score skipped",
			FeatureMeans: map[string]float64{}, FeatureStds: map[string]float64{},
		}
	}
	decoded, decodeRC := MaybeDecodeDistorted(
		ctx, request, opts.EncodeDir, opts.FFmpegBin, runners.Decode)
	if decodeRC != 0 {
		// Parity contract: score the unchanged container request so the row
		// records the vmaf failure instead of silently dropping the cell.
		slog.Warn("corpus: distorted decode failed; scoring original request",
			"distorted", request.Distorted, "exit_status", decodeRC)
	}
	return RunScore(ctx, decoded, opts.VMAFBin, runners.Score, "", opts.ScoreBackend)
}

func cleanupCellEncode(opts Options, result EncodeResult, out string) error {
	if opts.KeepEncodes || result.ExitStatus != 0 {
		return nil
	}
	if err := os.Remove(out); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("remove encoded output %q: %w", out, err)
	}
	return nil
}

// buildEncodeRequest assembles the per-cell EncodeRequest, including the
// cross-resolution scale filter and the HDR argv tail.
func buildEncodeRequest(
	job Job, opts Options, adapter *codecadapter.Adapter, cell Cell, out string,
	clip sampleClipPlan, hdrExtraParams []string,
) EncodeRequest {
	// ADR-0498 Bug #v2-B: when the caller supplied source dims distinct
	// from the rung target, tell ffmpeg the *source* geometry on the input
	// side (-s) and add a -vf scale=W:H filter so the encoded rendition
	// lands at the rung target.
	encSrcW, encSrcH := job.Width, job.Height
	if job.SrcWidth > 0 {
		encSrcW = job.SrcWidth
	}
	if job.SrcHeight > 0 {
		encSrcH = job.SrcHeight
	}

	// ADR-0505 Bug #V5-2: when the source is a container / Y4M the encode
	// pipe MUST treat it as a container and let ffmpeg auto-detect format
	// and resolution. The historic path always built the argv with
	// "-f rawvideo -pix_fmt … -s WxH", which reinterprets the container's
	// compressed bytes as planar YUV and produces a catastrophic encode
	// (~50 Mbps regardless of CRF, garbage frames, VMAF in the 4-9 band).
	sourceIsContainer := !IsRawYUVPath(job.Source)
	var scaleExtra []string
	switch {
	case sourceIsContainer:
		// Container sources: enforce the rung target unconditionally —
		// ffmpeg's auto-detected geometry may not match the requested
		// rendition. For native-geometry rungs the scale is a no-op.
		scaleExtra = []string{"-vf", fmt.Sprintf("scale=%d:%d", job.Width, job.Height)}
	case encSrcW != job.Width || encSrcH != job.Height:
		scaleExtra = []string{"-vf", fmt.Sprintf("scale=%d:%d", job.Width, job.Height)}
	}

	extra := make([]string, 0, len(hdrExtraParams)+len(scaleExtra))
	extra = append(extra, hdrExtraParams...)
	extra = append(extra, scaleExtra...)

	return EncodeRequest{
		Source:            job.Source,
		Width:             encSrcW,
		Height:            encSrcH,
		PixFmt:            job.PixFmt,
		Framerate:         job.Framerate,
		Encoder:           adapter.Encoder,
		Preset:            cell.Preset,
		CRF:               cell.CRF,
		Output:            out,
		ExtraParams:       extra,
		SampleClipSeconds: clip.ClipSeconds,
		SampleClipStartS:  clip.StartS,
		SourceIsContainer: sourceIsContainer,
		// ADR-0506 Bug #V6-1: plumb the analysed-window length so the
		// encode is bounded when the caller did not opt into sample-clip
		// mode (the ladder / CLI --duration flag exercises this path).
		DurationS: job.DurationS,
	}
}

// rowInput bundles everything buildRow needs for one corpus row.
type rowInput struct {
	Job        Job
	Opts       Options
	Cell       Cell
	SrcSHA     string
	Enc        EncodeResult
	Score      ScoreResult
	ScoreModel string
	ClipMode   string
	HDRInfo    *HdrInfo
	HDRForced  bool
	ShotMeta   ShotMetadata
}

// buildRow assembles one schema-v3 corpus row.
func buildRow(in rowInput) map[string]any {
	row := baseRow(in)
	addCanonicalFeatures(row, in.Score)
	for key, value := range AggregateStats(in.Enc.EncoderStats) {
		row[key] = value
	}
	return row
}

func baseRow(in rowInput) map[string]any {
	hdrTransfer, hdrPrimaries := hdrFields(in.HDRInfo)
	return map[string]any{
		"schema_version":        SchemaVersion,
		"run_id":                NewRunID(),
		"timestamp":             UTCNowISO8601(),
		"src":                   in.Job.Source,
		"src_sha256":            in.SrcSHA,
		"width":                 in.Job.Width,
		"height":                in.Job.Height,
		"pix_fmt":               in.Job.PixFmt,
		"framerate":             in.Job.Framerate,
		"duration_s":            in.Job.DurationS,
		"encoder":               in.Opts.Encoder,
		"encoder_version":       in.Enc.EncoderVersion,
		"preset":                in.Cell.Preset,
		"crf":                   in.Cell.CRF,
		"extra_params":          rowExtraParams(in.Enc.Request.ExtraParams),
		"encode_path":           rowEncodePath(in),
		"encode_size_bytes":     int(in.Enc.EncodeSizeBytes),
		"bitrate_kbps":          BitrateKbps(in.Enc.EncodeSizeBytes, encodedDuration(in)),
		"encode_time_ms":        in.Enc.EncodeTimeMS,
		"vmaf_score":            in.Score.VMAFScore,
		"vmaf_model":            in.ScoreModel,
		"score_time_ms":         in.Score.ScoreTimeMS,
		"ffmpeg_version":        in.Enc.FFmpegVersion,
		"vmaf_binary_version":   in.Score.VMAFBinaryVersion,
		"exit_status":           rowExitStatus(in),
		"clip_mode":             in.ClipMode,
		"hdr_transfer":          hdrTransfer,
		"hdr_primaries":         hdrPrimaries,
		"hdr_forced":            in.HDRForced,
		"shot_count":            in.ShotMeta.Count,
		"shot_avg_duration_sec": in.ShotMeta.AvgDurationSec,
		"shot_duration_std_sec": in.ShotMeta.DurationStdSec,
	}
}

func encodedDuration(in rowInput) float64 {
	if in.Enc.Request.SampleClipSeconds > 0.0 {
		return in.Enc.Request.SampleClipSeconds
	}
	return in.Job.DurationS
}

func rowExitStatus(in rowInput) int {
	if in.Enc.ExitStatus != 0 {
		return in.Enc.ExitStatus
	}
	return in.Score.ExitStatus
}

func rowEncodePath(in rowInput) string {
	if in.Opts.KeepEncodes {
		return in.Enc.Request.Output
	}
	return ""
}

func rowExtraParams(params []string) []string {
	if params == nil {
		return []string{}
	}
	return params
}

func hdrFields(info *HdrInfo) (string, string) {
	if info == nil {
		return "", ""
	}
	return info.Transfer, info.Primaries
}

// addCanonicalFeatures writes NaN for features the scorer did not expose.
func addCanonicalFeatures(row map[string]any, score ScoreResult) {
	for i, feature := range Canonical6Features {
		meanVal := math.NaN()
		if v, ok := score.FeatureMeans[feature]; ok {
			meanVal = v
		}
		stdVal := math.NaN()
		if v, ok := score.FeatureStds[feature]; ok {
			stdVal = v
		}
		row[Canonical6MeanKeys[i]] = meanVal
		row[Canonical6StdKeys[i]] = stdVal
	}
}

// MissingRowKeys reports which canonical keys a row is missing. It is the
// schema-shape assertion _row_for performs inline; exposed so the CLI and tests
// can check a row without duplicating the key list.
func MissingRowKeys(row map[string]any) []string {
	var missing []string
	for _, key := range RowKeys {
		if _, ok := row[key]; !ok {
			missing = append(missing, key)
		}
	}
	return missing
}
