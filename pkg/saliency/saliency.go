// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package saliency is the Go port of tools/vmaf-tune/src/vmaftune/
// saliency.py — saliency-aware ROI encoding (ADR-0287 / ADR-0293, companion
// to ADR-0237 and ADR-0286).
//
// Pipeline:
//
//	raw yuv420p -> sample N frames -> YUV->RGB -> ImageNet-normalise
//	  -> saliency_student_v1 -> mean saliency mask [0, 1]
//	  -> per-block reduce -> QP offsets [-12, +12]
//	  -> codec ROI sidecar/argv -> ffmpeg
//
// The numeric kernels (colour conversion, normalisation, temporal reduction,
// per-block reduce, QP clamp) are pure Go — no BLAS, no tensor library — so
// the whole pipeline except the single ONNX forward pass runs anywhere.
//
// The model inference itself is behind the Session seam. The Python original
// loads saliency_student_v1 with onnxruntime and degrades to a plain encode
// when onnxruntime or the model file is missing; the Go port keeps that exact
// posture, with the caller supplying the Session. See the package AGENTS.md
// note for how the CLI wires it.
package saliency

import (
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// DefaultModelRelPath is where the fork-trained student model lives, relative
// to the repo root.
const DefaultModelRelPath = "model/tiny/saliency_student_v1.onnx"

// QP offsets are clamped to a window both x264 and x265 accept comfortably
// (ADR-0247 sidecar convention).
const (
	QPOffsetMin = -12
	QPOffsetMax = 12
)

// Block granularities, in luma samples, for each encoder's ROI unit.
const (
	// X264MBSide is the x264 macroblock side, fixed by the codec.
	X264MBSide = 16
	// SVTAV1SBSide is SVT-AV1's super-block side.
	SVTAV1SBSide = 64
	// VVenCCTUSide is VVC's coding-tree-unit side at 1080p and above.
	VVenCCTUSide = 64
)

// ImageNet normalisation constants the saliency_student_v1 input layer
// expects (matching the C-side vmaf_tensor_from_rgb_imagenet helper).
var (
	imagenetMean = [3]float64{0.485, 0.456, 0.406}
	imagenetStd  = [3]float64{0.229, 0.224, 0.225}
)

// DefaultFrameSamples is how many frames a clip is reduced to for the
// aggregate mask — the same default the saliency scoring extractor uses.
const DefaultFrameSamples = 8

// Aggregator is the temporal reducer applied to the sampled per-frame masks.
type Aggregator string

const (
	// AggMean is the historical per-pixel arithmetic mean.
	AggMean Aggregator = "mean"
	// AggEMA is an exponential moving average with weight EMAAlpha on the
	// current frame.
	AggEMA Aggregator = "ema"
	// AggMax is the per-pixel maximum over sampled masks.
	AggMax Aggregator = "max"
	// AggMotionWeighted weights each frame by its mean luma delta from the
	// previous sampled frame.
	AggMotionWeighted Aggregator = "motion-weighted"
)

// Aggregators is the accepted set, in the CLI's declaration order.
var Aggregators = []Aggregator{AggMean, AggEMA, AggMax, AggMotionWeighted}

// DefaultAggregator and DefaultEMAAlpha mirror the Python defaults.
const (
	DefaultAggregator Aggregator = AggMean
	DefaultEMAAlpha              = 0.6
)

// ErrUnavailable is returned when saliency inference is requested but the
// model or the inference session is missing. Callers catch it to drop back to
// a non-saliency encode.
var ErrUnavailable = errors.New("saliency: inference unavailable")

// UnsupportedEncoderError is returned when saliency is requested for a codec
// that has no ROI dispatch and the caller has not opted in to the graceful
// fallback (ADR-0546). The CLI maps it to exit code 2.
type UnsupportedEncoderError struct {
	Encoder   string
	Supported []string
}

// Error renders the actionable message the CLI prints.
func (e *UnsupportedEncoderError) Error() string {
	return fmt.Sprintf(
		"vmaf-tune: saliency ROI is not implemented for encoder %q.\n"+
			"Supported encoders: %s\n"+
			"To accept a plain encode without ROI bias, pass "+
			"--saliency-fallback-plain or set VMAFTUNE_SALIENCY_FALLBACK_OK=1.",
		e.Encoder, strings.Join(e.Supported, ", "))
}

// Config carries the user-tunable knobs for the saliency-aware encode path.
type Config struct {
	// ForegroundOffset is the per-pixel QP offset applied at peak saliency.
	// Negative lowers QP (better quality) in salient regions.
	ForegroundOffset int
	// FrameSamples is how many frames are sampled across the clip. Higher is
	// more stable and slower.
	FrameSamples int
	// PersistSidecar writes the ROI sidecar next to the encode output
	// instead of into a temp file the caller deletes after the encode.
	PersistSidecar bool
	// TemporalAggregator reduces the sampled per-frame masks.
	TemporalAggregator Aggregator
	// EMAAlpha is the current-frame weight for AggEMA.
	EMAAlpha float64
	// AllowUnsupportedEncoderFallback demotes an unsupported encoder to a
	// plain encode instead of failing (ADR-0546). Set by
	// --saliency-fallback-plain or VMAFTUNE_SALIENCY_FALLBACK_OK=1.
	AllowUnsupportedEncoderFallback bool
}

// DefaultConfig mirrors the Python SaliencyConfig defaults.
func DefaultConfig() Config {
	return Config{
		ForegroundOffset:   -4,
		FrameSamples:       DefaultFrameSamples,
		TemporalAggregator: DefaultAggregator,
		EMAAlpha:           DefaultEMAAlpha,
	}
}

// Validate rejects an unknown aggregator or an out-of-range EMA weight.
func (c Config) Validate() error {
	known := false
	for _, a := range Aggregators {
		if c.TemporalAggregator == a {
			known = true
			break
		}
	}
	if !known {
		return fmt.Errorf(
			"saliency: temporal_aggregator must be one of %v, got %q",
			Aggregators, c.TemporalAggregator)
	}
	if !(c.EMAAlpha > 0.0 && c.EMAAlpha <= 1.0) {
		return fmt.Errorf("saliency: ema_alpha must be in (0, 1], got %v", c.EMAAlpha)
	}
	return nil
}

// Session runs one saliency_student_v1 forward pass.
//
// input is the NCHW [1, 3, H, W] tensor flattened row-major, already
// ImageNet-normalised and zero-padded to a multiple of 32 on both spatial
// axes. The return is the [1, 1, H, W] mask flattened the same way, in
// [0, 1].
//
// Padding is the caller's job because the model's UNet skip connections
// require both dimensions to be exact multiples of 32; ComputeMap pads before
// the call and crops the result afterwards.
type Session interface {
	Run(input []float32, height, width int) ([]float32, error)
}

// Frame420p holds one decoded yuv420p frame's planes.
type Frame420p struct {
	Y      []byte
	U      []byte
	V      []byte
	Width  int
	Height int
}

// FrameSizeBytes returns the raw 8-bit yuv420p frame size.
func FrameSizeBytes(width, height int) int {
	return width * height * 3 / 2
}

// FrameCount returns how many yuv420p frames of the given geometry fit in the
// file at path.
func FrameCount(path string, width, height int) (int, error) {
	info, err := os.Stat(path)
	if err != nil {
		return 0, fmt.Errorf("saliency: stat %q: %w", path, err)
	}
	size := FrameSizeBytes(width, height)
	if size <= 0 {
		return 0, fmt.Errorf("saliency: invalid geometry %dx%d", width, height)
	}
	return int(info.Size()) / size, nil
}

// ReadFrame reads one yuv420p frame at frameIndex from path.
func ReadFrame(path string, frameIndex, width, height int) (Frame420p, error) {
	frameBytes := FrameSizeBytes(width, height)
	lumaBytes := width * height
	chromaW, chromaH := width/2, height/2
	chromaBytes := chromaW * chromaH

	// #nosec G304 -- path is the raw YUV the caller decoded or supplied.
	fh, err := os.Open(path)
	if err != nil {
		return Frame420p{}, fmt.Errorf("saliency: open %q: %w", path, err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			_ = closeErr
		}
	}()

	buf := make([]byte, frameBytes)
	n, readErr := fh.ReadAt(buf, int64(frameIndex)*int64(frameBytes))
	if n != frameBytes {
		return Frame420p{}, fmt.Errorf(
			"saliency: short read on %s: wanted %d bytes at frame %d, got %d (%v)",
			path, frameBytes, frameIndex, n, readErr)
	}
	return Frame420p{
		Y:      buf[:lumaBytes],
		U:      buf[lumaBytes : lumaBytes+chromaBytes],
		V:      buf[lumaBytes+chromaBytes:],
		Width:  width,
		Height: height,
	}, nil
}

// ToRGBImageNet converts BT.709-limited yuv420p planes to the ImageNet-
// normalised float32 [1, 3, H, W] tensor the model expects, flattened
// row-major.
//
// Chroma is upsampled by pixel replication (the numpy repeat the Python
// original uses), the RGB is clipped to [0, 1] before normalisation, and the
// BT.709 matrix constants match the Python verbatim.
func (f Frame420p) ToRGBImageNet() []float32 {
	w, h := f.Width, f.Height
	chromaW := w / 2
	out := make([]float32, 3*h*w)
	planeStride := h * w

	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			yf := (float64(f.Y[y*w+x]) - 16.0) / 219.0
			ci := (y/2)*chromaW + (x / 2)
			uf := (float64(f.U[ci]) - 128.0) / 224.0
			vf := (float64(f.V[ci]) - 128.0) / 224.0

			r := yf + 1.5748*vf
			g := yf - 0.1873*uf - 0.4681*vf
			b := yf + 1.8556*uf

			idx := y*w + x
			for c, v := range [3]float64{r, g, b} {
				v = math.Max(0.0, math.Min(1.0, v))
				v = (v - imagenetMean[c]) / imagenetStd[c]
				out[c*planeStride+idx] = float32(v)
			}
		}
	}
	return out
}

// SampleFrameIndices picks n evenly-spaced frame indices in [0, total).
func SampleFrameIndices(total, n int) []int {
	if total <= 0 {
		return nil
	}
	if n <= 1 || total == 1 {
		return []int{0}
	}
	step := total / n
	if step < 1 {
		step = 1
	}
	idx := make([]int, 0, n)
	for i := 0; i < total && len(idx) < n; i += step {
		idx = append(idx, i)
	}
	if len(idx) == 0 {
		idx = []int{0}
	}
	return idx
}

// PadToMultiple zero-pads a [1, 3, H, W] flattened tensor to the next
// multiple of `multiple` on both spatial axes, returning the padded tensor
// and its padded dimensions.
//
// saliency_student_v1's UNet encoder-decoder uses skip connections that
// require both spatial dimensions to be exact multiples of 32; a source whose
// height or width is not causes a shape mismatch inside the runtime when the
// encoder feature map cannot be concatenated with the decoder's upsampled
// tensor.
func PadToMultiple(tensor []float32, height, width, multiple int) (padded []float32, padH, padW int) {
	padH = ((height + multiple - 1) / multiple) * multiple
	padW = ((width + multiple - 1) / multiple) * multiple
	if padH == height && padW == width {
		return tensor, height, width
	}
	out := make([]float32, 3*padH*padW)
	for c := 0; c < 3; c++ {
		srcPlane := c * height * width
		dstPlane := c * padH * padW
		for y := 0; y < height; y++ {
			copy(out[dstPlane+y*padW:dstPlane+y*padW+width],
				tensor[srcPlane+y*width:srcPlane+y*width+width])
		}
	}
	return out, padH, padW
}

// MapOptions configures ComputeMap.
type MapOptions struct {
	FrameSamples       int
	TemporalAggregator Aggregator
	EMAAlpha           float64
}

// ComputeMap runs the saliency model over a sampled subset of frames and
// reduces them to one [H, W] aggregate mask in [0, 1], row-major.
//
// A height that is not a multiple of 8 is rejected up front with an
// actionable message: the model's encoder path downsamples by 8, and an
// off-by-one tensor shape surfaces as a cryptic runtime error otherwise.
func ComputeMap(videoPath string, width, height int, session Session, opts MapOptions) ([]float64, error) {
	if session == nil {
		return nil, fmt.Errorf("%w: no inference session supplied", ErrUnavailable)
	}
	if height%8 != 0 {
		return nil, fmt.Errorf(
			"saliency: height %d is not divisible by 8. The saliency_student_v1 "+
				"encoder path requires height %% 8 == 0. Pad or crop the source to "+
				"the next multiple of 8 (e.g. %d) before calling this function.",
			height, ((height+7)/8)*8)
	}
	cfg := Config{
		TemporalAggregator: opts.TemporalAggregator,
		EMAAlpha:           opts.EMAAlpha,
	}
	if err := cfg.Validate(); err != nil {
		return nil, err
	}

	nframes, err := FrameCount(videoPath, width, height)
	if err != nil {
		return nil, err
	}
	if nframes <= 0 {
		return nil, fmt.Errorf(
			"saliency: no frames in %s for %dx%d yuv420p", videoPath, width, height)
	}
	samples := opts.FrameSamples
	if samples <= 0 {
		samples = DefaultFrameSamples
	}
	indices := SampleFrameIndices(nframes, samples)

	pixels := width * height
	accum := make([]float64, pixels)
	maxMask := make([]float64, pixels)
	var emaMask []float64
	weightSum := 0.0
	var prevY []byte

	for _, fi := range indices {
		frame, readErr := ReadFrame(videoPath, fi, width, height)
		if readErr != nil {
			return nil, readErr
		}
		tensor := frame.ToRGBImageNet()
		padded, padH, padW := PadToMultiple(tensor, height, width, 32)

		raw, runErr := session.Run(padded, padH, padW)
		if runErr != nil {
			return nil, fmt.Errorf("%w: %v", ErrUnavailable, runErr)
		}
		if len(raw) < padH*padW {
			return nil, fmt.Errorf(
				"saliency: model returned %d values, want at least %d for a %dx%d mask",
				len(raw), padH*padW, padW, padH)
		}
		// Crop the padded mask back to the source geometry.
		mask := make([]float64, pixels)
		for y := 0; y < height; y++ {
			for x := 0; x < width; x++ {
				mask[y*width+x] = float64(raw[y*padW+x])
			}
		}

		switch cfg.TemporalAggregator {
		case AggMean:
			for i, v := range mask {
				accum[i] += v
			}
		case AggMax:
			for i, v := range mask {
				if v > maxMask[i] {
					maxMask[i] = v
				}
			}
		case AggEMA:
			if emaMask == nil {
				emaMask = append([]float64(nil), mask...)
			} else {
				for i, v := range mask {
					emaMask[i] = cfg.EMAAlpha*v + (1.0-cfg.EMAAlpha)*emaMask[i]
				}
			}
		case AggMotionWeighted:
			weight := MotionWeight(prevY, frame.Y)
			for i, v := range mask {
				accum[i] += v * weight
			}
			weightSum += weight
			prevY = append([]byte(nil), frame.Y...)
		}
	}

	var result []float64
	switch cfg.TemporalAggregator {
	case AggMean:
		n := float64(len(indices))
		result = accum
		for i := range result {
			result[i] /= n
		}
	case AggMax:
		result = maxMask
	case AggEMA:
		result = emaMask
		if result == nil {
			result = accum
		}
	case AggMotionWeighted:
		result = accum
		for i := range result {
			result[i] /= weightSum
		}
	}
	// Pin to [0, 1] against FP drift on the boundary.
	for i := range result {
		result[i] = math.Max(0.0, math.Min(1.0, result[i]))
	}
	return result, nil
}

// MotionWeight returns a non-zero saliency weight from luma motion energy:
// the mean absolute frame delta, normalised to [0, 1] and floored so a static
// frame still contributes.
func MotionWeight(prevY, y []byte) float64 {
	if prevY == nil || len(prevY) != len(y) || len(y) == 0 {
		return 1.0
	}
	sum := 0.0
	for i := range y {
		sum += math.Abs(float64(y[i]) - float64(prevY[i]))
	}
	return math.Max(sum/float64(len(y))/255.0, 1.0e-6)
}

// ToQPMap maps a saliency mask onto per-pixel QP offsets.
//
// Convention (matching the vmaf-roi sidecar, ADR-0247):
//
//	saliency 1.0 -> foregroundOffset          (typically negative: more bits)
//	saliency 0.0 -> -foregroundOffset         (background: fewer bits)
//	saliency 0.5 -> 0
//
// The baseline QP is deliberately NOT folded in: these are deltas the encoder
// applies on top of its own rate-control decision.
func ToQPMap(mask []float64, foregroundOffset int) []int {
	out := make([]int, len(mask))
	for i, v := range mask {
		centred := v*2.0 - 1.0
		offset := math.Round(centred * float64(foregroundOffset))
		out[i] = int(clampFloat(offset, QPOffsetMin, QPOffsetMax))
	}
	return out
}

// ReduceToBlocks reduces a per-pixel QP-offset map to per-block means,
// rounded and clamped. Block-mean keeps the offsets locally smooth, which
// matters because a too-noisy sidecar gets rejected by some encoders.
//
// The map is cropped to whole blocks; a partial trailing block is dropped,
// matching the Python reshape.
func ReduceToBlocks(qpMap []int, width, height, block int) ([][]int, error) {
	if block <= 0 {
		return nil, fmt.Errorf("saliency: block size must be positive, got %d", block)
	}
	bh, bw := height/block, width/block
	if bh == 0 || bw == 0 {
		return nil, fmt.Errorf(
			"saliency: qp map %dx%d smaller than block size %dx%d",
			height, width, block, block)
	}
	out := make([][]int, bh)
	blockArea := float64(block * block)
	for by := 0; by < bh; by++ {
		row := make([]int, bw)
		for bx := 0; bx < bw; bx++ {
			sum := 0.0
			for y := by * block; y < (by+1)*block; y++ {
				base := y * width
				for x := bx * block; x < (bx+1)*block; x++ {
					sum += float64(qpMap[base+x])
				}
			}
			row[bx] = int(clampFloat(math.Round(sum/blockArea), QPOffsetMin, QPOffsetMax))
		}
		out[by] = row
	}
	return out, nil
}

// X264QPFile renders an x264 --qpfile for a durationFrames-long clip.
//
// Format: one "index type qp" header line per frame, followed by one line per
// macroblock row carrying that row's deltas. The same per-MB pattern is
// emitted for every frame because the mask is a per-clip aggregate. The first
// frame is typed I so x264 anchors the GOP; the preset still owns the actual
// GOP structure.
func X264QPFile(blockOffsets [][]int, durationFrames int) string {
	var sb strings.Builder
	for frameIdx := 0; frameIdx < durationFrames; frameIdx++ {
		kind := "P"
		if frameIdx == 0 {
			kind = "I"
		}
		fmt.Fprintf(&sb, "%d %s 0\n", frameIdx, kind)
		for _, row := range blockOffsets {
			sb.WriteString(joinInts(row, " "))
			sb.WriteString("\n")
		}
	}
	return sb.String()
}

// SVTAV1QPOffsetMap renders SVT-AV1's --qp-file: space-separated rows of
// super-block offsets, one block per frame, frames separated by a blank line.
//
// Format reference: SVT-AV1 Source/App/EncApp/EbAppConfig.c read_qp_map_file
// at tag v2.1.0.
func SVTAV1QPOffsetMap(blockOffsets [][]int, durationFrames int) string {
	return framesJoined(blockOffsets, durationFrames, " ")
}

// VVenCROICSV renders VVenC's --roi map: comma-separated rows of CTU offsets,
// frames separated by a blank line. Positive delta lowers quality, negative
// raises it — the same convention ToQPMap uses.
//
// Format reference: VVenC source/Lib/apputils/VVEncAppCfg.cpp parseROIFile at
// tag v1.14.0.
func VVenCROICSV(blockOffsets [][]int, durationFrames int) string {
	return framesJoined(blockOffsets, durationFrames, ",")
}

// framesJoined renders one block grid repeated durationFrames times, rows
// joined by sep and frames separated by a blank line.
func framesJoined(blockOffsets [][]int, durationFrames int, sep string) string {
	rows := make([]string, len(blockOffsets))
	for i, row := range blockOffsets {
		rows[i] = joinInts(row, sep)
	}
	frameBlock := strings.Join(rows, "\n")
	frames := make([]string, durationFrames)
	for i := range frames {
		frames[i] = frameBlock
	}
	return strings.Join(frames, "\n\n") + "\n"
}

// X265ZonesArg derives a single spatial-aggregate QP delta from the block map
// and replicates it across the clip as one --zones entry.
//
// x265's zones syntax is "startfrm,endfrm,q=<int>", where q is a QP DELTA on
// top of the encoder's own rate-control decision — the same semantics as
// x264's qpfile. Negative delta means more bits in salient regions.
//
// A per-zone temporal partition would be the richer form; the per-clip
// spatial aggregate matches the x264 posture and is enough for first-order
// ROI wins (ADR-0287 bucket #2).
func X265ZonesArg(blockOffsets [][]int, durationFrames int) string {
	sum, count := 0.0, 0
	for _, row := range blockOffsets {
		for _, v := range row {
			sum += float64(v)
			count++
		}
	}
	meanOffset := 0
	if count > 0 {
		meanOffset = int(clampFloat(math.Round(sum/float64(count)), QPOffsetMin, QPOffsetMax))
	}
	lastFrame := durationFrames - 1
	if lastFrame < 0 {
		lastFrame = 0
	}
	return fmt.Sprintf("0,%d,q=%d", lastFrame, meanOffset)
}

// joinInts renders a row of ints with the given separator.
func joinInts(values []int, sep string) string {
	parts := make([]string, len(values))
	for i, v := range values {
		parts[i] = strconv.Itoa(v)
	}
	return strings.Join(parts, sep)
}

// clampFloat bounds v to [lo, hi].
func clampFloat(v float64, lo, hi int) float64 {
	return math.Max(float64(lo), math.Min(float64(hi), v))
}

// Augment describes the ROI wiring for one encode: the extra ffmpeg params to
// splice in, plus the sidecar file (if any) that must be written first and,
// when ephemeral, deleted after the encode.
type Augment struct {
	ExtraParams []string
	// SidecarPath is empty for encoders whose ROI rides on argv alone
	// (libx265's zones string).
	SidecarPath string
	// SidecarBody is the file content to write at SidecarPath.
	SidecarBody string
	// SidecarSuffix is the extension used when a persisted sidecar is named
	// after the encode output.
	SidecarSuffix string
}

// supportedEncoders is the ROI dispatch table, sorted for a stable error
// message.
var supportedEncoders = []string{
	"libaom-av1", "libsvtav1", "libvvenc", "libx264", "libx265",
}

// SupportedEncoders returns the codecs with a saliency ROI dispatch.
func SupportedEncoders() []string {
	out := make([]string, len(supportedEncoders))
	copy(out, supportedEncoders)
	return out
}

// BuildAugment produces the per-codec ROI wiring from a per-pixel QP map.
//
// Each encoder reduces the pixel map to its own native ROI granularity:
// 16x16 macroblocks for x264 and the patched libaom bridge, a spatial mean
// for x265 zones, 64x64 super-blocks for SVT-AV1, 64x64 CTUs for VVenC.
func BuildAugment(encoder string, qpMap []int, width, height, durationFrames int) (Augment, error) {
	switch encoder {
	case "libx264":
		blocks, err := ReduceToBlocks(qpMap, width, height, X264MBSide)
		if err != nil {
			return Augment{}, err
		}
		return Augment{
			SidecarBody:   X264QPFile(blocks, durationFrames),
			SidecarSuffix: ".qpfile.txt",
		}, nil

	case "libaom-av1":
		// The fork's FFmpeg patch stack teaches libaom-av1 to consume the
		// same x264-style qpfile, mapping each 16x16 macroblock delta onto
		// libaom's mode-info grid and segment-QP table. Unlike x264 the path
		// arrives through a top-level -qpfile AVOption, not an opaque
		// encoder-params key.
		blocks, err := ReduceToBlocks(qpMap, width, height, X264MBSide)
		if err != nil {
			return Augment{}, err
		}
		return Augment{
			SidecarBody:   X264QPFile(blocks, durationFrames),
			SidecarSuffix: ".libaom-qpfile.txt",
		}, nil

	case "libx265":
		blocks, err := ReduceToBlocks(qpMap, width, height, X264MBSide)
		if err != nil {
			return Augment{}, err
		}
		zones := X265ZonesArg(blocks, durationFrames)
		return Augment{
			ExtraParams: []string{"-x265-params", "zones=" + zones},
		}, nil

	case "libsvtav1":
		blocks, err := ReduceToBlocks(qpMap, width, height, SVTAV1SBSide)
		if err != nil {
			return Augment{}, err
		}
		return Augment{
			SidecarBody:   SVTAV1QPOffsetMap(blocks, durationFrames),
			SidecarSuffix: ".svtav1-qpmap.txt",
		}, nil

	case "libvvenc":
		blocks, err := ReduceToBlocks(qpMap, width, height, VVenCCTUSide)
		if err != nil {
			return Augment{}, err
		}
		return Augment{
			SidecarBody:   VVenCROICSV(blocks, durationFrames),
			SidecarSuffix: ".vvenc-roi.csv",
		}, nil

	default:
		supported := SupportedEncoders()
		sort.Strings(supported)
		return Augment{}, &UnsupportedEncoderError{Encoder: encoder, Supported: supported}
	}
}

// ExtraParamsFor returns the ffmpeg params that reference a written sidecar
// at path, for the given encoder.
func ExtraParamsFor(encoder, sidecarPath string) []string {
	switch encoder {
	case "libx264":
		// x264 takes the path through the opaque encoder-params channel.
		return []string{"-x264-params", "qpfile=" + sidecarPath}
	case "libaom-av1":
		// The fork's patch stack exposes a shared top-level -qpfile AVOption.
		return []string{"-qpfile", sidecarPath}
	case "libsvtav1":
		// FFmpeg's libsvtav1 wrapper forwards opaque key=value pairs to
		// EbConfig via svt_av1_enc_set_parameter; qp-file is the documented
		// SVT-AV1 config name (EbAppConfig.c v2.1.0).
		return []string{"-svtav1-params", "qp-file=" + sidecarPath}
	case "libvvenc":
		// ROIFile is defined in VVEncAppCfg.h at tag v1.14.0.
		return []string{"-vvenc-params", "ROIFile=" + sidecarPath}
	default:
		return nil
	}
}

// WriteSidecar writes the augment's sidecar body to path, creating parent
// directories. It is a no-op for argv-only encoders.
func WriteSidecar(a Augment, path string) error {
	if a.SidecarBody == "" {
		return nil
	}
	// G301: 0o750 keeps the sidecar directory owner+group accessible only.
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("saliency: create sidecar dir: %w", err)
	}
	// G306: 0o600 — the sidecar encodes the content's saliency structure.
	if err := os.WriteFile(path, []byte(a.SidecarBody), 0o600); err != nil {
		return fmt.Errorf("saliency: write sidecar %q: %w", path, err)
	}
	return nil
}

// PersistedSidecarPath names a persisted sidecar after the encode output,
// mirroring the Python output.with_suffix(...) convention.
func PersistedSidecarPath(outputPath, suffix string) string {
	ext := filepath.Ext(outputPath)
	return strings.TrimSuffix(outputPath, ext) + suffix
}

// FallbackAllowed reports whether an unsupported encoder may be demoted to a
// plain encode, honouring both the config flag and the env override
// (ADR-0546).
func FallbackAllowed(cfg Config) bool {
	return cfg.AllowUnsupportedEncoderFallback ||
		strings.TrimSpace(os.Getenv("VMAFTUNE_SALIENCY_FALLBACK_OK")) == "1"
}
