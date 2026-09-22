// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/libvmaf/direct.go — direct libvmaf cgo scoring path (ADR-0931 Phase 1).
//
// This file implements `ScoreDirect`, the in-process replacement for the
// `Scorer.Score` subprocess-delegation path defined in libvmaf.go.  See
// docs/adr/0931-mcp-cgo-direct-replace-subprocess.md for the design
// contract — in particular the C-function order, picture-ownership
// transfer rules, and error mapping.
//
// Phase 1 scope (CPU-only, yuv420p/yuv422p/yuv444p, 8/10/12-bit):
//   - vmaf_init           → context
//   - vmaf_model_load_from_path → model
//   - vmaf_use_features_from_model
//   - per-frame: vmaf_picture_alloc x2, read planes, vmaf_read_pictures
//   - flush: vmaf_read_pictures(NULL, NULL, 0)
//   - vmaf_score_pooled (MEAN)
//   - cleanup: vmaf_model_destroy + vmaf_close
//
// Locale: setlocale(LC_NUMERIC, "C") is invoked once via init() — ADR-0137.

//go:build cgo

package libvmaf

/*
#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

// score_direct_set_locale_c is invoked from Go init() once per process to
// guarantee LC_NUMERIC=C before any libvmaf function reads or writes a
// floating-point string (ADR-0137).  Calling setlocale() multiple times is
// safe — it returns the previous locale string and idempotently applies the
// new one.
static void score_direct_set_locale_c(void) {
    setlocale(LC_NUMERIC, "C");
}
*/
import "C"

import (
	"context"
	"fmt"
	"io"
	"os"
	"sync"
	"unsafe"
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

func (p PoolMethod) toC() (C.enum_VmafPoolingMethod, error) {
	switch p {
	case PoolMethodMin:
		return C.VMAF_POOL_METHOD_MIN, nil
	case PoolMethodMax:
		return C.VMAF_POOL_METHOD_MAX, nil
	case PoolMethodMean, PoolMethodUnknown:
		return C.VMAF_POOL_METHOD_MEAN, nil
	case PoolMethodHarmonicMean:
		return C.VMAF_POOL_METHOD_HARMONIC_MEAN, nil
	case PoolMethodMedian:
		return C.VMAF_POOL_METHOD_MEDIAN, nil
	case PoolMethodPerc5:
		return C.VMAF_POOL_METHOD_PERC5, nil
	case PoolMethodPerc10:
		return C.VMAF_POOL_METHOD_PERC10, nil
	case PoolMethodPerc20:
		return C.VMAF_POOL_METHOD_PERC20, nil
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
}

// init runs once per process to pin LC_NUMERIC=C before any libvmaf scoring
// call (ADR-0137).  Idempotent.
func init() {
	C.score_direct_set_locale_c()
}

// directOnce protects the one-time logging of "direct path selected" so the
// operator sees the choice without a per-call spam.
var directOnce sync.Once

// LogDirectPathSelected emits a one-shot INFO-level marker that the direct
// cgo scoring path is being used.  The MCP tool handlers call this on the
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
// in-process libvmaf cgo path.  Returns ErrInvalidArgument / ErrOutOfMemory
// / ErrModelNotFound / ErrPictureRead via fmt.Errorf chains; the subprocess
// fallback path remains the caller's responsibility.
//
// Goroutine safety: each call constructs a fresh VmafContext + VmafModel and
// destroys them on return.  Concurrent ScoreDirect calls are safe — libvmaf
// is thread-safe at the per-context level since 2.0.0.
//
// The supplied ctx governs cancellation of the per-frame read+queue loop.
// libvmaf itself has no cancellation API, so cancellation is checked at frame
// boundaries: when ctx.Done() fires we unref any half-allocated pictures,
// abandon the loop, and let the deferred vmaf_close / vmaf_model_destroy
// release the context cleanly.  Cancelled calls return ctx.Err() wrapped
// in fmt.Errorf so callers can use errors.Is(err, context.Canceled).
// Passing nil ctx is treated as context.Background().
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
func ScoreDirect(ctx context.Context, req ScoreDirectRequest) (*ScoreDirectResult, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := ctx.Err(); err != nil {
		return nil, fmt.Errorf("ScoreDirect: context cancelled before start: %w", err)
	}
	if err := validateScoreDirectRequest(req); err != nil {
		return nil, err
	}

	var vmafCtx *C.VmafContext
	rc := C.vmaf_init(&vmafCtx, newScoringConfiguration())
	if err := mapErrno("vmaf_init", int(rc)); err != nil {
		return nil, err
	}
	defer C.vmaf_close(vmafCtx)

	model, err := loadModelFromPath(req.ModelPath, "vmaf_direct")
	if err != nil {
		return nil, err
	}
	defer C.vmaf_model_destroy(model)

	// vmaf_use_features_from_model registers the feature extractors the
	// model's predictor needs.
	rc = C.vmaf_use_features_from_model(vmafCtx, model)
	if err := mapErrno("vmaf_use_features_from_model", int(rc)); err != nil {
		return nil, err
	}

	refF, err := os.Open(req.Ref) //nolint:gosec // path is operator-supplied
	if err != nil {
		return nil, fmt.Errorf("ScoreDirect: open ref %q: %w", req.Ref, ErrPictureRead)
	}
	defer refF.Close()
	disF, err := os.Open(req.Dis) //nolint:gosec // path is operator-supplied
	if err != nil {
		return nil, fmt.Errorf("ScoreDirect: open dis %q: %w", req.Dis, ErrPictureRead)
	}
	defer disF.Close()

	frameIdx, err := feedDirectFrames(ctx, vmafCtx, refF, disF, newDirectGeometry(req))
	if err != nil {
		return nil, err
	}
	if frameIdx == 0 {
		return nil, fmt.Errorf("ScoreDirect: zero frames read from %s / %s: %w",
			req.Ref, req.Dis, ErrPictureRead)
	}
	if err := flushDirectFrames(vmafCtx); err != nil {
		return nil, err
	}
	score, err := poolDirectScore(vmafCtx, model, req.PoolMethod, frameIdx)
	if err != nil {
		return nil, err
	}
	return &ScoreDirectResult{VMAF: score, FrameCount: frameIdx, Backend: "cpu"}, nil
}

// validateScoreDirectRequest fails fast with typed errors rather than relying
// on libvmaf's -EINVAL, so callers get an actionable message before any C
// resource is allocated.  Extracted from ScoreDirect unchanged.
func validateScoreDirectRequest(req ScoreDirectRequest) error {
	if req.Ref == "" || req.Dis == "" {
		return fmt.Errorf("ScoreDirect: ref and dis are required: %w", ErrInvalidArgument)
	}
	if req.ModelPath == "" {
		return fmt.Errorf("ScoreDirect: model_path is required: %w", ErrInvalidArgument)
	}
	if req.Width <= 0 || req.Height <= 0 {
		return fmt.Errorf("ScoreDirect: width/height must be positive: %w", ErrInvalidArgument)
	}
	if req.PixFmt == 0 {
		return fmt.Errorf("ScoreDirect: pix_fmt is required: %w", ErrInvalidArgument)
	}
	if req.BitDepth != 8 && req.BitDepth != 10 && req.BitDepth != 12 {
		return fmt.Errorf("ScoreDirect: bit_depth must be 8/10/12, got %d: %w",
			req.BitDepth, ErrInvalidArgument)
	}
	if _, err := os.Stat(req.ModelPath); os.IsNotExist(err) {
		return fmt.Errorf("ScoreDirect: model %q: %w", req.ModelPath, ErrModelNotFound)
	}
	return nil
}

// loadModelFromPath loads the JSON model at modelPath through libvmaf under
// the given informational model name.  The caller owns the returned model and
// must vmaf_model_destroy it.
//
// The configured name is informational only — libvmaf copies it
// (vmaf_model_generate_name mallocs its own buffer) — and modelPath is read
// synchronously by vmaf_read_json_model_from_path, so both C strings are
// released before this function returns.
func loadModelFromPath(modelPath, modelName string) (*C.VmafModel, error) {
	cModelPath := C.CString(modelPath)
	// SAFETY: cModelPath is the C.CString malloc'd on the line above; it is
	// never re-assigned and has no other owner, so the deferred free releases
	// a live block exactly once, after the only call that reads it.
	defer C.free(unsafe.Pointer(cModelPath))
	cModelName := C.CString(modelName)
	// SAFETY: cModelName is the C.CString malloc'd on the line above; libvmaf
	// copies the name into the model, so freeing it here cannot dangle.
	defer C.free(unsafe.Pointer(cModelName))
	mcfg := C.VmafModelConfig{
		name:  cModelName,
		flags: C.VMAF_MODEL_FLAGS_DEFAULT,
	}
	var model *C.VmafModel
	rc := C.vmaf_model_load_from_path(&model, &mcfg, cModelPath)
	if err := mapErrno("vmaf_model_load_from_path", int(rc)); err != nil {
		return nil, err
	}
	return model, nil
}

// newScoringConfiguration returns the VmafConfiguration both in-process
// scoring entry points use: warning-level logging, libvmaf's own thread
// auto-selection, no subsampling and no CPU/GPU feature masking.
func newScoringConfiguration() C.VmafConfiguration {
	return C.VmafConfiguration{
		log_level:   C.VMAF_LOG_LEVEL_WARNING,
		n_threads:   0,
		n_subsample: 1,
		cpumask:     0,
		gpumask:     0,
	}
}

// directGeometry bundles the per-frame C geometry so the read+queue loop can
// be expressed without re-deriving it on every iteration.
type directGeometry struct {
	pixFmt    C.enum_VmafPixelFormat
	bpc       C.uint
	width     C.uint
	height    C.uint
	frameSize int
}

// newDirectGeometry converts the Go-side request geometry into the C types
// vmaf_picture_alloc expects, plus the on-disk frame size.
func newDirectGeometry(req ScoreDirectRequest) directGeometry {
	return directGeometry{
		pixFmt:    C.enum_VmafPixelFormat(req.PixFmt),
		bpc:       C.uint(req.BitDepth),
		width:     C.uint(req.Width),
		height:    C.uint(req.Height),
		frameSize: frameBytes(req.Width, req.Height, req.PixFmt, req.BitDepth),
	}
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
// the caller's deferred vmaf_close + vmaf_model_destroy release whatever
// state libvmaf accumulated; the un-flushed frames simply never reach
// vmaf_score_pooled.
func feedDirectFrames(
	ctx context.Context,
	vmafCtx *C.VmafContext,
	refF, disF *os.File,
	geom directGeometry,
) (int, error) {
	for frameIdx := 0; frameIdx <= maxDirectFrames; frameIdx++ {
		if err := ctx.Err(); err != nil {
			return 0, fmt.Errorf("ScoreDirect: cancelled at frame %d: %w", frameIdx, err)
		}
		atEOF, err := readAndQueueDirectFrame(vmafCtx, refF, disF, geom, frameIdx)
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

// readAndQueueDirectFrame allocates one ref/dis picture pair, fills it from
// the two readers and transfers ownership to libvmaf.  It reports true when
// both streams hit a clean end-of-file on a frame boundary.
//
// vmaf_picture_alloc allocates the data planes via posix_memalign; ownership
// transfers to libvmaf on the vmaf_read_pictures call below.  On any error
// before that call we MUST vmaf_picture_unref both pics to avoid a leak.
func readAndQueueDirectFrame(
	vmafCtx *C.VmafContext,
	refF, disF *os.File,
	geom directGeometry,
	frameIdx int,
) (bool, error) {
	var refPic, disPic C.VmafPicture
	if rc := C.vmaf_picture_alloc(&refPic, geom.pixFmt, geom.bpc, geom.width, geom.height); rc != 0 {
		return false, mapErrno("vmaf_picture_alloc(ref)", int(rc))
	}
	if rc := C.vmaf_picture_alloc(&disPic, geom.pixFmt, geom.bpc, geom.width, geom.height); rc != 0 {
		C.vmaf_picture_unref(&refPic)
		return false, mapErrno("vmaf_picture_alloc(dis)", int(rc))
	}
	nRef, refErr := readFrameInto(refF, &refPic, geom.frameSize)
	nDis, disErr := readFrameInto(disF, &disPic, geom.frameSize)
	atEOF, err := classifyDirectFrameRead(nRef, nDis, refErr, disErr, geom.frameSize, frameIdx)
	if atEOF || err != nil {
		C.vmaf_picture_unref(&refPic)
		C.vmaf_picture_unref(&disPic)
		return atEOF, err
	}
	// Transfer ownership.  libvmaf calls vmaf_picture_unref internally
	// after the read; we MUST NOT call it again from Go.
	rc := C.vmaf_read_pictures(vmafCtx, &refPic, &disPic, C.uint(frameIdx))
	if readErr := mapErrno("vmaf_read_pictures", int(rc)); readErr != nil {
		return false, readErr
	}
	return false, nil
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

// flushDirectFrames signals end-of-stream with vmaf_read_pictures(NULL, NULL, 0)
// so the feature extractors finalise their internal buffers.
func flushDirectFrames(vmafCtx *C.VmafContext) error {
	rc := C.vmaf_read_pictures(vmafCtx, nil, nil, 0)
	return mapErrno("vmaf_read_pictures(flush)", int(rc))
}

// poolDirectScore pools VMAF over [0, frameIdx-1] with the requested method.
func poolDirectScore(
	vmafCtx *C.VmafContext,
	model *C.VmafModel,
	method PoolMethod,
	frameIdx int,
) (float64, error) {
	cPoolMethod, err := method.toC()
	if err != nil {
		return 0, fmt.Errorf("ScoreDirect: %w: %v", ErrInvalidArgument, err)
	}
	var score C.double
	rc := C.vmaf_score_pooled(vmafCtx, model, cPoolMethod, &score, 0, C.uint(frameIdx-1))
	if err := mapErrno("vmaf_score_pooled", int(rc)); err != nil {
		return 0, err
	}
	return float64(score), nil
}

// ValidateModel opens path via vmaf_model_load_from_path and immediately
// destroys the resulting model.  Useful as a syntactic check that the file
// loads through libvmaf — catches schema drift the pure-JSON parser silently
// ignores (missing required fields, unknown model_type, etc.).
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
	var model *C.VmafModel
	cPath := C.CString(path)
	// SAFETY: cPath is the C.CString malloc'd on the line above; it is never
	// re-assigned and has no other owner, so the deferred free releases a
	// live block exactly once.
	defer C.free(unsafe.Pointer(cPath))
	cName := C.CString("validate")
	// SAFETY: cName is the C.CString malloc'd on the line above.  libvmaf
	// copies the configured name (vmaf_model_generate_name mallocs its own
	// buffer), so freeing it here cannot dangle inside the model.
	defer C.free(unsafe.Pointer(cName))
	mcfg := C.VmafModelConfig{
		name:  cName,
		flags: C.VMAF_MODEL_FLAGS_DEFAULT,
	}
	rc := C.vmaf_model_load_from_path(&model, &mcfg, cPath)
	if err := mapErrno("vmaf_model_load_from_path", int(rc)); err != nil {
		return err
	}
	C.vmaf_model_destroy(model)
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

// readFrameInto reads one frame's worth of raw planar YUV from r into the
// pre-allocated planes of pic.  Returns the number of bytes read across all
// planes and the first I/O error encountered (io.EOF for clean end-of-file).
//
// The function reads each plane in turn into pic.data[i], respecting the
// libvmaf stride (which equals the row width for tightly-packed pictures
// allocated via vmaf_picture_alloc).  bytesPerSample == 2 for >8-bit.
func readFrameInto(r io.Reader, pic *C.VmafPicture, frameSize int) (int, error) {
	total := 0
	bytesPerSample := 1
	if pic.bpc > 8 {
		bytesPerSample = 2
	}
	for plane := 0; plane < 3; plane++ {
		w := int(pic.w[plane])
		h := int(pic.h[plane])
		stride := int(pic.stride[plane])
		dataPtr := pic.data[plane]
		if dataPtr == nil || w == 0 || h == 0 {
			continue
		}
		rowBytes := w * bytesPerSample
		// vmaf_picture_alloc allocates stride >= rowBytes; for each row we
		// read rowBytes from disk and copy into the stride-aligned slot.
		// Read row-by-row so packed-on-disk YUV maps correctly into the
		// strided in-memory layout libvmaf prefers.
		// Use a Go-side scratch buffer to avoid touching the C heap with
		// io.ReadFull; copy via runtime memmove via unsafe.Slice.
		scratch := make([]byte, rowBytes)
		// SAFETY: dataPtr is the plane base returned by vmaf_picture_alloc,
		// which guarantees stride[plane]*h[plane] readable-and-writable bytes
		// for that plane; the nil/zero-extent case was rejected above, and
		// the slice never outlives pic, whose planes libvmaf keeps alive
		// until the ownership transfer in vmaf_read_pictures.
		dst := unsafe.Slice((*byte)(dataPtr), stride*h)
		for row := 0; row < h; row++ {
			n, err := io.ReadFull(r, scratch)
			total += n
			if err != nil {
				return total, err
			}
			copy(dst[row*stride:row*stride+rowBytes], scratch)
		}
	}
	if total != frameSize {
		// Defensive: should be caught by the io.ReadFull loop above, but
		// surface the mismatch explicitly so callers can diagnose stride
		// vs. file-layout mismatches.
		return total, fmt.Errorf("frame-size mismatch: read %d want %d", total, frameSize)
	}
	return total, nil
}
