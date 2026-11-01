// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/libvmaf/direct.go — direct in-process scoring path (ADR-0931 Phase 1).
//
// This file implements `ScoreDirect`, the in-process replacement for the
// `Scorer.Score` subprocess-delegation path defined in libvmaf.go.  See
// docs/adr/0931-mcp-cgo-direct-replace-subprocess.md for the design
// contract — in particular the call order, frame-ownership transfer rules,
// and error mapping.
//
// #2434: the native calls run on the generated pkg/vmafx binding through the
// session in engine.go; this file holds no cgo.  The call order is unchanged:
//   - vmafx_context_create       → context
//   - vmafx_model_load_file      → model
//   - vmafx_context_use_model    → registers the model's extractors
//   - per-frame: vmafx_frame_create_host x2, read planes, vmafx_submit
//   - flush: vmafx_flush
//   - vmafx_score_pooled (MEAN)
//   - cleanup: retryable context close, then the model reference is dropped
//
// Locale: LC_NUMERIC=C is pinned once per process by pkg/vmafx — ADR-0137.

//go:build cgo

package libvmaf

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/pkg/vmafx"
)

// PixelFormat names mirror the libvmaf enum VmafPixelFormat without exposing
// cgo types to Go callers.
type PixelFormat int

const (
	PixFmtYUV420P PixelFormat = 1
	PixFmtYUV422P PixelFormat = 2
	PixFmtYUV444P PixelFormat = 3
)

// String returns the human-readable name of the pixel format.
func (p PixelFormat) String() string {
	switch p {
	case PixFmtYUV420P:
		return "yuv420p"
	case PixFmtYUV422P:
		return "yuv422p"
	case PixFmtYUV444P:
		return "yuv444p"
	default:
		return fmt.Sprintf("unknown(%d)", int(p))
	}
}

// ParsePixFmt converts the MCP-level pixfmt string ("420"/"422"/"444") into
// a PixelFormat.  Accepts the longer "yuv420p" form as well.
func ParsePixFmt(s string) (PixelFormat, error) {
	switch s {
	case "420", "yuv420p":
		return PixFmtYUV420P, nil
	case "422", "yuv422p":
		return PixFmtYUV422P, nil
	case "444", "yuv444p":
		return PixFmtYUV444P, nil
	default:
		return 0, fmt.Errorf("libvmaf: unsupported pixel format %q (want 420/422/444)", s)
	}
}

// PoolMethod mirrors enum VmafPoolingMethod without exposing cgo types to Go callers.
type PoolMethod int

const (
	PoolMethodUnknown      PoolMethod = 0
	PoolMethodMin          PoolMethod = 1
	PoolMethodMax          PoolMethod = 2
	PoolMethodMean         PoolMethod = 3
	PoolMethodHarmonicMean PoolMethod = 4
	PoolMethodMedian       PoolMethod = 5
	PoolMethodPerc5        PoolMethod = 6
	PoolMethodPerc10       PoolMethod = 7
	PoolMethodPerc20       PoolMethod = 8
)

// String returns the human-readable name of the pooling method.
func (p PoolMethod) String() string {
	switch p {
	case PoolMethodMin:
		return "min"
	case PoolMethodMax:
		return "max"
	case PoolMethodMean:
		return "mean"
	case PoolMethodHarmonicMean:
		return "harmonic_mean"
	case PoolMethodMedian:
		return "median"
	case PoolMethodPerc5:
		return "perc5"
	case PoolMethodPerc10:
		return "perc10"
	case PoolMethodPerc20:
		return "perc20"
	default:
		return fmt.Sprintf("unknown(%d)", int(p))
	}
}

// ParsePoolMethod converts a string into a PoolMethod. Empty string defaults to PoolMethodMean.
func ParsePoolMethod(s string) (PoolMethod, error) {
	switch s {
	case "min":
		return PoolMethodMin, nil
	case "max":
		return PoolMethodMax, nil
	case "mean", "":
		return PoolMethodMean, nil
	case "harmonic_mean":
		return PoolMethodHarmonicMean, nil
	case "median":
		return PoolMethodMedian, nil
	case "perc5":
		return PoolMethodPerc5, nil
	case "perc10":
		return PoolMethodPerc10, nil
	case "perc20":
		return PoolMethodPerc20, nil
	default:
		return 0, fmt.Errorf("libvmaf: unsupported pooling method %q", s)
	}
}

// toPool returns the pkg/vmafx pooling constant of the method; the integer
// values of PoolMethod mirror enum VmafPoolingMethod and VmafxPool.
func (p PoolMethod) toPool() (uint32, error) {
	switch p {
	case PoolMethodMin:
		return vmafx.PoolMin, nil
	case PoolMethodMax:
		return vmafx.PoolMax, nil
	case PoolMethodMean, PoolMethodUnknown:
		return vmafx.PoolMean, nil
	case PoolMethodHarmonicMean:
		return vmafx.PoolHarmonicMean, nil
	case PoolMethodMedian:
		return vmafx.PoolMedian, nil
	case PoolMethodPerc5:
		return vmafx.PoolPerc5, nil
	case PoolMethodPerc10:
		return vmafx.PoolPerc10, nil
	case PoolMethodPerc20:
		return vmafx.PoolPerc20, nil
	default:
		return 0, fmt.Errorf("libvmaf: unsupported pooling method %d", int(p))
	}
}

// ScoreDirectRequest carries the parameters for the direct-cgo scoring path.
// Fields mirror the `vmaf_score` MCP tool's input schema.
//
// The struct is intentionally separate from the subprocess Scorer config so
// the two paths can evolve independently.  Defaults are applied in
// ScoreDirect; zero-valued required fields produce a typed error.
type ScoreDirectRequest struct {
	// Ref is the absolute path to the reference YUV file.  Must be readable.
	Ref string
	// Dis is the absolute path to the distorted YUV file.  Must be readable.
	Dis string
	// ModelPath is the absolute path to the VMAF model JSON.  The Phase 1
	// path supports SVM models (.json / .pkl) only; DNN models route through
	// the subprocess path until Phase 3 lands the ONNX cgo bridge.
	ModelPath string
	// Width / Height of each frame in pixels.
	Width, Height int
	// PixFmt is the chroma subsampling layout.
	PixFmt PixelFormat
	// BitDepth is 8, 10, or 12.  10/12 use 16-bit planes per libvmaf
	// convention (bpc).
	BitDepth int
	// PoolMethod selects the temporal pooling strategy. Zero / unset defaults to PoolMethodMean.
	PoolMethod PoolMethod
}

// ScoreDirectResult mirrors the subset of subprocess-path JSON the MCP server
// surfaces today: the mean pooled VMAF + the frame count.  Per-feature pooled
// scores are deferred to Phase 2.
type ScoreDirectResult struct {
	// VMAF is the pooled-mean VMAF score over the full sequence.
	VMAF float64
	// FrameCount is the number of frame pairs successfully scored.
	FrameCount int
	// Backend is always "cpu" in Phase 1.  Phase 2 extends to GPU backends
	// via cpumask / gpumask + the backend-specific runtime init calls.
	Backend string
	// Provenance is the context's provenance record (#2142, RC4 WP5), the
	// JSON the CLI report's "provenance" object carries.
	Provenance json.RawMessage
}

// directOnce protects the one-time logging of "direct path selected" so the
// operator sees the choice without a per-call spam.
var directOnce sync.Once

// LogDirectPathSelected emits a one-shot INFO-level marker that the direct
// in-process scoring path is being used.  The MCP tool handlers call this on the
// first dispatched request when VMAFX_MCP_DIRECT=1 is set.
//
// The marker is deliberately written to stderr (not slog) so it appears in
// the same stream as the existing subprocess-path error output; downstream
// log routers can re-classify if needed.
func LogDirectPathSelected() {
	directOnce.Do(func() {
		fmt.Fprintln(os.Stderr,
			"libvmaf: VMAFX_MCP_DIRECT=1 — using in-process cgo scoring path (ADR-0931)")
	})
}

// ScoreDirect computes a VMAF score for the (ref, dis) pair using the
// in-process libvmaf path (pkg/vmafx).  Returns ErrInvalidArgument / ErrOutOfMemory
// / ErrModelNotFound / ErrPictureRead via fmt.Errorf chains; the subprocess
// fallback path remains the caller's responsibility.
//
// Goroutine safety: each call constructs a fresh VmafxContext + VmafxModel and
// closes the context before dropping its model.  A close failure gets one
// immediate retry; persistent failure is returned and the owners are left
// allocated rather than freeing a dependency still referenced by the context.
// Concurrent ScoreDirect calls are safe — libvmaf is thread-safe at the
// per-context level since 2.0.0.
//
// The supplied ctx governs cancellation of the per-frame read+queue loop.
// libvmaf itself has no cancellation API, so cancellation is checked at frame
// boundaries: when ctx.Done() fires we release any half-filled frames,
// abandon the loop, and let the deferred ordered owner close the context then
// drop the model.  Cancelled calls return ctx.Err() wrapped
// in fmt.Errorf so callers can use errors.Is(err, context.Canceled).
func openDirectPair(ref, dis string) (*os.File, *os.File, error) {
	// #nosec G304 -- the caller's input path; the MCP handlers validate it against the allow-list first.
	refF, err := os.Open(ref) //nolint:gosec // path is operator-supplied
	if err != nil {
		return nil, nil, fmt.Errorf("ScoreDirect: open ref %q: %w", ref, ErrPictureRead)
	}
	// #nosec G304 -- the caller's input path; the MCP handlers validate it against the allow-list first.
	disF, err := os.Open(dis) //nolint:gosec // path is operator-supplied
	if err != nil {
		closeInput(refF)
		return nil, nil, fmt.Errorf("ScoreDirect: open dis %q: %w", dis, ErrPictureRead)
	}
	return refF, disF, nil
}

// closeInput closes a read-only input file; a failure cannot change the score,
// so it is logged, not returned.
func closeInput(f *os.File) {
	if err := f.Close(); err != nil {
		slog.Debug("libvmaf: close input", "error", err)
	}
}

func withDefaultDirectTimeout(ctx context.Context) (context.Context, context.CancelFunc) {
	if ctx == nil {
		ctx = context.Background()
	}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		return context.WithTimeout(ctx, 30*time.Minute)
	}
	return ctx, func() {}
}

// ScoreDirect evaluates VMAF directly over two raw YUV files through the
// generated pkg/vmafx binding.
func ScoreDirect(ctx context.Context, req ScoreDirectRequest) (
	result *ScoreDirectResult, retErr error,
) {
	runCtx, cancel := withDefaultDirectTimeout(ctx)
	defer cancel()
	if err := runCtx.Err(); err != nil {
		return nil, fmt.Errorf("ScoreDirect: context cancelled before start: %w", err)
	}
	if err := validateScoreDirectRequest(req); err != nil {
		return nil, err
	}

	session, err := openScoringSession("ScoreDirect", req.ModelPath, "vmaf_direct")
	if err != nil {
		return nil, err
	}
	defer func() {
		if closeErr := session.owner.closeWithImmediateRetry(); closeErr != nil {
			result = nil
			retErr = errors.Join(retErr, fmt.Errorf("ScoreDirect cleanup: %w", closeErr))
		}
	}()
	return scoreDirectFrames(runCtx, session, req)
}

// scoreDirectFrames feeds the two files through an open session and reads the
// pooled score and the provenance record back.
func scoreDirectFrames(
	ctx context.Context, session *scoringSession, req ScoreDirectRequest,
) (*ScoreDirectResult, error) {
	refF, disF, err := openDirectPair(req.Ref, req.Dis)
	if err != nil {
		return nil, err
	}
	defer closeInput(refF)
	defer closeInput(disF)

	geom := newFrameGeometry(req.Width, req.Height, req.PixFmt, req.BitDepth)
	frameIdx, err := feedDirectFrames(ctx, session, refF, disF, geom)
	if err != nil {
		return nil, err
	}
	if frameIdx == 0 {
		return nil, fmt.Errorf("ScoreDirect: zero frames read from %s / %s: %w",
			req.Ref, req.Dis, ErrPictureRead)
	}
	if err := session.flush(); err != nil {
		return nil, err
	}
	score, err := session.pooled(req.PoolMethod, frameIdx)
	if err != nil {
		return nil, fmt.Errorf("ScoreDirect: %w", err)
	}
	provenance, err := session.provenance()
	if err != nil {
		return nil, fmt.Errorf("ScoreDirect: %w", err)
	}
	return &ScoreDirectResult{
		VMAF: score, FrameCount: frameIdx, Backend: "cpu", Provenance: json.RawMessage(provenance),
	}, nil
}

// validateScoreDirectRequest fails fast with typed errors rather than relying
// on libvmaf's -EINVAL, so callers get an actionable message before any
// native resource is allocated.  The geometry and model checks are shared with
// NewStreamScorer (validate.go).
func validateScoreDirectRequest(req ScoreDirectRequest) error {
	if req.Ref == "" || req.Dis == "" {
		return fmt.Errorf("ScoreDirect: ref and dis are required: %w", ErrInvalidArgument)
	}
	if err := requireModelPath("ScoreDirect", req.ModelPath); err != nil {
		return err
	}
	if err := requireGeometry("ScoreDirect", req.Width, req.Height, req.PixFmt, req.BitDepth); err != nil {
		return err
	}
	return requireModelExists("ScoreDirect", req.ModelPath)
}

// maxDirectFrames bounds the per-frame read+queue loop (HISS-02).  libvmaf
// itself imposes no frame ceiling, so the bound is derived from the largest
// sequence the direct path can plausibly be handed: 2^20 frames is just over
// twelve hours at 24 fps, and at the smallest supported geometry already
// exceeds the addressable size of any single YUV file we accept.  Exhausting
// it means the input stream never reached EOF, which is reported as a read
// error rather than silently truncating the score.
const maxDirectFrames = 1 << 20

// feedDirectFrames runs the per-frame read+queue loop and returns the number
// of frame pairs handed to libvmaf.  Flushing stays with the caller so the
// zero-frame case is still reported before libvmaf sees an end-of-stream.
//
// Cancellation is checked at frame boundaries — libvmaf has no cancellation
// API, so this is the only place the loop can bail out.  Returning here lets
// the caller's deferred ordered owner release whatever state libvmaf
// accumulated; the un-flushed frames simply never reach the pooled score.
func feedDirectFrames(
	ctx context.Context,
	session *scoringSession,
	refF, disF *os.File,
	geom frameGeometry,
) (int, error) {
	for frameIdx := 0; frameIdx <= maxDirectFrames; frameIdx++ {
		if err := ctx.Err(); err != nil {
			return 0, fmt.Errorf("ScoreDirect: cancelled at frame %d: %w", frameIdx, err)
		}
		atEOF, err := readAndQueueDirectFrame(session, refF, disF, geom, frameIdx)
		if err != nil {
			return 0, err
		}
		if atEOF {
			return frameIdx, nil
		}
	}
	return 0, fmt.Errorf(
		"ScoreDirect: input exceeded the %d-frame bound without reaching EOF: %w",
		maxDirectFrames, ErrPictureRead)
}

// readAndQueueDirectFrame allocates one ref/dis frame pair, fills it from
// the two readers and transfers ownership to the engine.  It reports true when
// both streams hit a clean end-of-file on a frame boundary.
//
// vmafx_submit consumes both frames on every path, so the deferred Close calls
// release a frame only when it never reached the engine (an end-of-file, a
// read error, or a failed fill); after the submit they are no-ops.
func readAndQueueDirectFrame(
	session *scoringSession,
	refF, disF *os.File,
	geom frameGeometry,
	frameIdx int,
) (bool, error) {
	refFrame, disFrame, err := newFramePair(geom.desc)
	if err != nil {
		return false, err
	}
	defer refFrame.Close()
	defer disFrame.Close()
	nRef, refErr := fillFrame(refFrame, refF)
	nDis, disErr := fillFrame(disFrame, disF)
	atEOF, err := classifyDirectFrameRead(nRef, nDis, refErr, disErr, geom.frameSize, frameIdx)
	if atEOF || err != nil {
		return atEOF, err
	}
	return false, session.submitPair(refFrame, disFrame, frameIdx)
}

// classifyDirectFrameRead turns the two per-plane read outcomes into the
// loop's decision: clean end-of-stream, a malformed input, or "frame is
// complete, queue it".  EOF on the first read of a frame ends the loop
// cleanly; EOF mid-frame is a malformed input.
func classifyDirectFrameRead(
	nRef, nDis int,
	refErr, disErr error,
	frameSize, frameIdx int,
) (bool, error) {
	if refErr == io.EOF && disErr == io.EOF && nRef == 0 && nDis == 0 {
		return true, nil
	}
	if refErr != nil && refErr != io.EOF {
		return false, fmt.Errorf("ScoreDirect: read ref frame %d: %w", frameIdx, ErrPictureRead)
	}
	if disErr != nil && disErr != io.EOF {
		return false, fmt.Errorf("ScoreDirect: read dis frame %d: %w", frameIdx, ErrPictureRead)
	}
	if nRef != frameSize || nDis != frameSize {
		return false, fmt.Errorf(
			"ScoreDirect: short read on frame %d (ref=%d dis=%d want=%d): %w",
			frameIdx, nRef, nDis, frameSize, ErrPictureRead)
	}
	return false, nil
}

// ValidateModel loads path through libvmaf and immediately releases the
// resulting model.  Useful as a syntactic check that the file loads — catches
// schema drift the pure-JSON parser silently ignores (missing required
// fields, unknown model_type, etc.).
//
// Returns nil on success; on failure returns the typed error (ErrModelNotFound,
// ErrInvalidArgument, ...).  Used by the MCP describe_model handler when
// VMAFX_MCP_DIRECT=1 is set.
func ValidateModel(path string) error {
	if path == "" {
		return fmt.Errorf("ValidateModel: empty path: %w", ErrInvalidArgument)
	}
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return fmt.Errorf("ValidateModel: %q: %w", path, ErrModelNotFound)
	}
	model, err := loadScoringModel(path, "validate")
	if err != nil {
		return err
	}
	model.Close()
	return nil
}

// frameBytes returns the on-disk size of one frame in bytes for the given
// geometry.  Phase 1 supports planar YUV with the standard chroma subsampling
// ratios; 10/12-bit are 2 bytes per sample (libvmaf's bpc convention).
func frameBytes(w, h int, pf PixelFormat, bitDepth int) int {
	luma := w * h
	var chroma int
	switch pf {
	case PixFmtYUV420P:
		chroma = (w / 2) * (h / 2) * 2 // Cb + Cr
	case PixFmtYUV422P:
		chroma = (w / 2) * h * 2
	case PixFmtYUV444P:
		chroma = w * h * 2
	}
	bytesPerSample := 1
	if bitDepth > 8 {
		bytesPerSample = 2
	}
	return (luma + chroma) * bytesPerSample
}
