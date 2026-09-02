// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package saliency_test

import (
	"errors"
	"math"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/saliency"
)

// TestToQPMap_matchesPythonGolden pins the saliency -> QP-offset mapping
// against the Python saliency_to_qp_map for the same masks.
//
// The convention under test: saliency 1.0 maps to foregroundOffset (negative
// = more bits), 0.0 to its negation, 0.5 to zero.
func TestToQPMap_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	mask := []float64{0.0, 0.25, 0.5, 0.75, 1.0}
	tests := []struct {
		name             string
		foregroundOffset int
		want             []int
	}{
		{"default -4", -4, []int{4, 2, 0, -2, -4}},
		{"maximum -12", -12, []int{12, 6, 0, -6, -12}},
		{"inverted +4", 4, []int{-4, -2, 0, 2, 4}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := saliency.ToQPMap(mask, tc.foregroundOffset); !slices.Equal(got, tc.want) {
				t.Errorf("ToQPMap = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestToQPMap_clamps asserts an out-of-band gain still lands inside the
// encoder-accepted window.
func TestToQPMap_clamps(t *testing.T) {
	t.Parallel()

	got := saliency.ToQPMap([]float64{0.0, 1.0}, -40)
	want := []int{saliency.QPOffsetMax, saliency.QPOffsetMin}
	if !slices.Equal(got, want) {
		t.Errorf("ToQPMap with an out-of-band gain = %v, want %v", got, want)
	}
}

// TestReduceToBlocks_matchesPythonGolden pins the block-mean reduce against
// the Python reduce_qp_map_to_blocks on a 32x48 pseudo-random map.
func TestReduceToBlocks_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	got, err := saliency.ReduceToBlocks(goldenReduceInput, 48, 32, saliency.X264MBSide)
	if err != nil {
		t.Fatalf("ReduceToBlocks: %v", err)
	}
	if len(got) != len(goldenReduceBlock16) {
		t.Fatalf("block rows = %d, want %d", len(got), len(goldenReduceBlock16))
	}
	for i := range got {
		if !slices.Equal(got[i], goldenReduceBlock16[i]) {
			t.Errorf("block row %d = %v, want %v", i, got[i], goldenReduceBlock16[i])
		}
	}
}

// TestReduceToBlocks_errors covers the geometry guards.
func TestReduceToBlocks_errors(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		width, height int
		block         int
		wantErr       bool
	}{
		{"map smaller than one block", 8, 8, 16, true},
		{"zero block size", 32, 32, 0, true},
		{"negative block size", 32, 32, -16, true},
		{"exact fit", 16, 16, 16, false},
		{"partial trailing block is cropped", 40, 40, 16, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			qpMap := make([]int, tc.width*tc.height)
			_, err := saliency.ReduceToBlocks(qpMap, tc.width, tc.height, tc.block)
			if (err != nil) != tc.wantErr {
				t.Errorf("ReduceToBlocks error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestFormatters_matchPythonGolden pins the four sidecar / argv renderers
// byte for byte. These files are consumed by x264, libaom, SVT-AV1 and VVenC
// directly, so a stray separator or a missing trailing newline is a real
// encode failure, not cosmetic.
func TestFormatters_matchPythonGolden(t *testing.T) {
	t.Parallel()

	t.Run("x264 qpfile", func(t *testing.T) {
		t.Parallel()

		if got := saliency.X264QPFile(goldenBlocks, 2); got != goldenX264QPFile {
			t.Errorf("X264QPFile = %q, want %q", got, goldenX264QPFile)
		}
	})

	t.Run("svt-av1 qp map", func(t *testing.T) {
		t.Parallel()

		if got := saliency.SVTAV1QPOffsetMap(goldenBlocks, 2); got != goldenSVTAV1QPMap {
			t.Errorf("SVTAV1QPOffsetMap = %q, want %q", got, goldenSVTAV1QPMap)
		}
	})

	t.Run("vvenc roi csv", func(t *testing.T) {
		t.Parallel()

		if got := saliency.VVenCROICSV(goldenBlocks, 2); got != goldenVVenCROICSV {
			t.Errorf("VVenCROICSV = %q, want %q", got, goldenVVenCROICSV)
		}
	})

	t.Run("x265 zones", func(t *testing.T) {
		t.Parallel()

		if got := saliency.X265ZonesArg(goldenBlocks, 48); got != goldenX265Zones {
			t.Errorf("X265ZonesArg = %q, want %q", got, goldenX265Zones)
		}
	})
}

// TestX265ZonesArg_edgeCases covers the zero-frame and empty-grid guards.
func TestX265ZonesArg_edgeCases(t *testing.T) {
	t.Parallel()

	if got := saliency.X265ZonesArg(goldenBlocks, 0); got != "0,0,q=-1" {
		t.Errorf("zero-frame zones = %q, want 0,0,q=-1", got)
	}
	if got := saliency.X265ZonesArg(nil, 10); got != "0,9,q=0" {
		t.Errorf("empty-grid zones = %q, want 0,9,q=0", got)
	}
}

// TestToRGBImageNet_matchesPythonGolden pins the colour conversion, chroma
// upsample, clipping and ImageNet normalisation against the NumPy original
// on a synthetic 8x8 frame.
func TestToRGBImageNet_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	frame := saliency.Frame420p{
		Y: goldenYUVY, U: goldenYUVU, V: goldenYUVV,
		Width: 8, Height: 8,
	}
	got := frame.ToRGBImageNet()
	if len(got) != len(goldenYUVTensor) {
		t.Fatalf("tensor length = %d, want %d", len(got), len(goldenYUVTensor))
	}
	for i := range got {
		if math.Abs(float64(got[i]-goldenYUVTensor[i])) > 1e-5 {
			t.Fatalf("tensor[%d] = %v, want %v", i, got[i], goldenYUVTensor[i])
		}
	}
}

// TestSampleFrameIndices_matchesPythonGolden pins the even-spacing sampler.
func TestSampleFrameIndices_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		total int
		n     int
		want  []int
	}{
		{"long clip", 100, 8, []int{0, 12, 24, 36, 48, 60, 72, 84}},
		{"fewer frames than samples", 5, 8, []int{0, 1, 2, 3, 4}},
		{"single frame", 1, 8, []int{0}},
		{"no frames", 0, 8, nil},
		{"exactly the sample count", 8, 8, []int{0, 1, 2, 3, 4, 5, 6, 7}},
		{"one sample", 8, 1, []int{0}},
		{"uneven spacing truncates to n", 7, 3, []int{0, 2, 4}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := saliency.SampleFrameIndices(tc.total, tc.n)
			if !slices.Equal(got, tc.want) {
				t.Errorf("SampleFrameIndices(%d, %d) = %v, want %v",
					tc.total, tc.n, got, tc.want)
			}
		})
	}
}

// TestMotionWeight covers the no-previous-frame case and the normalised
// delta.
func TestMotionWeight(t *testing.T) {
	t.Parallel()

	// 51 / 255 = 0.2
	prev := make([]byte, 16)
	cur := make([]byte, 16)
	for i := range cur {
		cur[i] = 51
	}
	if got := saliency.MotionWeight(prev, cur); math.Abs(got-0.2) > 1e-9 {
		t.Errorf("MotionWeight = %v, want 0.2", got)
	}
	if got := saliency.MotionWeight(nil, cur); got != 1.0 {
		t.Errorf("MotionWeight without a previous frame = %v, want 1.0", got)
	}
	// A static frame must still contribute a non-zero weight, or the
	// motion-weighted aggregator divides by zero.
	if got := saliency.MotionWeight(cur, cur); got <= 0.0 {
		t.Errorf("MotionWeight for a static frame = %v, want > 0", got)
	}
	if got := saliency.MotionWeight(prev[:4], cur); got != 1.0 {
		t.Errorf("MotionWeight with mismatched lengths = %v, want 1.0", got)
	}
}

// TestPadToMultiple covers the pass-through and the zero-pad paths.
func TestPadToMultiple(t *testing.T) {
	t.Parallel()

	t.Run("already aligned is a pass-through", func(t *testing.T) {
		t.Parallel()

		tensor := make([]float32, 3*32*64)
		for i := range tensor {
			tensor[i] = float32(i)
		}
		got, h, w := saliency.PadToMultiple(tensor, 32, 64, 32)
		if h != 32 || w != 64 {
			t.Errorf("padded dims = %dx%d, want 32x64", h, w)
		}
		if len(got) != len(tensor) {
			t.Errorf("length changed on an aligned tensor")
		}
	})

	t.Run("unaligned is zero-padded with the content preserved", func(t *testing.T) {
		t.Parallel()

		const srcH, srcW = 40, 40
		tensor := make([]float32, 3*srcH*srcW)
		for i := range tensor {
			tensor[i] = 1.0
		}
		got, h, w := saliency.PadToMultiple(tensor, srcH, srcW, 32)
		if h != 64 || w != 64 {
			t.Fatalf("padded dims = %dx%d, want 64x64", h, w)
		}
		if len(got) != 3*64*64 {
			t.Fatalf("padded length = %d, want %d", len(got), 3*64*64)
		}
		// Original region carries the content...
		for c := 0; c < 3; c++ {
			for y := 0; y < srcH; y++ {
				for x := 0; x < srcW; x++ {
					if got[c*64*64+y*64+x] != 1.0 {
						t.Fatalf("content lost at (%d, %d, %d)", c, y, x)
					}
				}
			}
		}
		// ...and the padding is zero.
		if got[40] != 0.0 || got[64*40] != 0.0 {
			t.Error("padding should be zero")
		}
	})
}

// stubSession returns a constant mask, or an error.
type stubSession struct {
	value float32
	err   error
	// lastH / lastW record the padded geometry the session was handed.
	lastH, lastW int
	calls        int
}

func (s *stubSession) Run(input []float32, height, width int) ([]float32, error) {
	s.calls++
	s.lastH, s.lastW = height, width
	if s.err != nil {
		return nil, s.err
	}
	if len(input) != 3*height*width {
		return nil, errors.New("stub: input arity does not match the padded geometry")
	}
	out := make([]float32, height*width)
	for i := range out {
		out[i] = s.value
	}
	return out, nil
}

// writeYUV writes n identical yuv420p frames of the given geometry.
func writeYUV(t *testing.T, dir string, width, height, frames int, luma byte) string {
	t.Helper()

	path := filepath.Join(dir, "src.yuv")
	frameSize := saliency.FrameSizeBytes(width, height)
	buf := make([]byte, frameSize*frames)
	for i := range buf {
		buf[i] = luma
	}
	if err := os.WriteFile(path, buf, 0o600); err != nil {
		t.Fatalf("write yuv: %v", err)
	}
	return path
}

// TestComputeMap covers the four aggregators, the geometry guard, and the
// missing-session degradation.
func TestComputeMap(t *testing.T) {
	t.Parallel()

	const w, h = 64, 64

	t.Run("mean aggregator", func(t *testing.T) {
		t.Parallel()

		path := writeYUV(t, t.TempDir(), w, h, 4, 128)
		session := &stubSession{value: 0.5}
		mask, err := saliency.ComputeMap(path, w, h, session, saliency.MapOptions{
			FrameSamples: 4, TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
		})
		if err != nil {
			t.Fatalf("ComputeMap: %v", err)
		}
		if len(mask) != w*h {
			t.Fatalf("mask length = %d, want %d", len(mask), w*h)
		}
		for _, v := range mask {
			if math.Abs(v-0.5) > 1e-6 {
				t.Fatalf("mask value = %v, want 0.5", v)
			}
		}
		if session.calls != 4 {
			t.Errorf("session calls = %d, want 4", session.calls)
		}
	})

	for _, agg := range []saliency.Aggregator{
		saliency.AggEMA, saliency.AggMax, saliency.AggMotionWeighted,
	} {
		t.Run(string(agg)+" aggregator", func(t *testing.T) {
			t.Parallel()

			path := writeYUV(t, t.TempDir(), w, h, 4, 128)
			mask, err := saliency.ComputeMap(path, w, h, &stubSession{value: 0.75},
				saliency.MapOptions{
					FrameSamples: 4, TemporalAggregator: agg, EMAAlpha: 0.6,
				})
			if err != nil {
				t.Fatalf("ComputeMap: %v", err)
			}
			// Every aggregator of a constant 0.75 mask is 0.75.
			for _, v := range mask {
				if math.Abs(v-0.75) > 1e-6 {
					t.Fatalf("%s mask value = %v, want 0.75", agg, v)
				}
			}
		})
	}

	t.Run("pads unaligned geometry before inference", func(t *testing.T) {
		t.Parallel()

		// 40 is divisible by 8 (so it passes the guard) but not by 32.
		const uw, uh = 40, 40
		path := writeYUV(t, t.TempDir(), uw, uh, 1, 128)
		session := &stubSession{value: 0.5}
		if _, err := saliency.ComputeMap(path, uw, uh, session, saliency.MapOptions{
			FrameSamples: 1, TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
		}); err != nil {
			t.Fatalf("ComputeMap: %v", err)
		}
		if session.lastH != 64 || session.lastW != 64 {
			t.Errorf("session saw %dx%d, want the 32-aligned 64x64",
				session.lastW, session.lastH)
		}
	})

	t.Run("errors", func(t *testing.T) {
		t.Parallel()

		dir := t.TempDir()
		path := writeYUV(t, dir, w, h, 2, 128)

		tests := []struct {
			name    string
			path    string
			width   int
			height  int
			session saliency.Session
			opts    saliency.MapOptions
		}{
			{
				name: "no session", path: path, width: w, height: h,
				session: nil,
				opts: saliency.MapOptions{
					TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
				},
			},
			{
				name: "height not divisible by 8", path: path, width: w, height: 70,
				session: &stubSession{value: 0.5},
				opts: saliency.MapOptions{
					TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
				},
			},
			{
				name: "unknown aggregator", path: path, width: w, height: h,
				session: &stubSession{value: 0.5},
				opts: saliency.MapOptions{
					TemporalAggregator: "median", EMAAlpha: 0.6,
				},
			},
			{
				name: "ema alpha out of range", path: path, width: w, height: h,
				session: &stubSession{value: 0.5},
				opts: saliency.MapOptions{
					TemporalAggregator: saliency.AggEMA, EMAAlpha: 0.0,
				},
			},
			{
				name: "missing file", path: filepath.Join(dir, "nope.yuv"),
				width: w, height: h, session: &stubSession{value: 0.5},
				opts: saliency.MapOptions{
					TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
				},
			},
			{
				name: "inference failure", path: path, width: w, height: h,
				session: &stubSession{err: errors.New("ort exploded")},
				opts: saliency.MapOptions{
					TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
				},
			},
		}
		for _, tc := range tests {
			t.Run(tc.name, func(t *testing.T) {
				t.Parallel()

				_, err := saliency.ComputeMap(
					tc.path, tc.width, tc.height, tc.session, tc.opts)
				if err == nil {
					t.Error("expected an error")
				}
			})
		}
	})
}

// TestComputeMap_unavailableIsDetectable asserts the no-session and
// inference-failure paths report ErrUnavailable, which is what the CLI
// branches on to fall back to a plain encode.
func TestComputeMap_unavailableIsDetectable(t *testing.T) {
	t.Parallel()

	path := writeYUV(t, t.TempDir(), 64, 64, 1, 128)
	opts := saliency.MapOptions{
		FrameSamples: 1, TemporalAggregator: saliency.AggMean, EMAAlpha: 0.6,
	}

	_, noSessionErr := saliency.ComputeMap(path, 64, 64, nil, opts)
	if !errors.Is(noSessionErr, saliency.ErrUnavailable) {
		t.Errorf("no-session error = %v, want it to wrap ErrUnavailable", noSessionErr)
	}

	_, inferErr := saliency.ComputeMap(path, 64, 64,
		&stubSession{err: errors.New("boom")}, opts)
	if !errors.Is(inferErr, saliency.ErrUnavailable) {
		t.Errorf("inference error = %v, want it to wrap ErrUnavailable", inferErr)
	}
}

// TestBuildAugment covers each codec's ROI dispatch plus the unsupported
// case, which must carry the actionable ADR-0546 message.
func TestBuildAugment(t *testing.T) {
	t.Parallel()

	const w, h = 128, 128
	qpMap := make([]int, w*h)
	for i := range qpMap {
		qpMap[i] = -4
	}

	tests := []struct {
		name           string
		encoder        string
		wantSuffix     string
		wantExtraParam string
		wantBodyPrefix string
		wantErr        bool
	}{
		{
			name: "libx264 writes a qpfile", encoder: "libx264",
			wantSuffix: ".qpfile.txt", wantBodyPrefix: "0 I 0\n",
		},
		{
			name: "libaom uses the patched qpfile bridge", encoder: "libaom-av1",
			wantSuffix: ".libaom-qpfile.txt", wantBodyPrefix: "0 I 0\n",
		},
		{
			name: "libx265 rides on argv", encoder: "libx265",
			wantExtraParam: "-x265-params",
		},
		{
			name: "libsvtav1 writes a super-block map", encoder: "libsvtav1",
			wantSuffix: ".svtav1-qpmap.txt", wantBodyPrefix: "-4 -4",
		},
		{
			name: "libvvenc writes a CTU csv", encoder: "libvvenc",
			wantSuffix: ".vvenc-roi.csv", wantBodyPrefix: "-4,-4",
		},
		{
			name: "hardware encoders have no ROI dispatch", encoder: "h264_nvenc",
			wantErr: true,
		},
		{
			name: "libvpx has no ROI dispatch", encoder: "libvpx-vp9",
			wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := saliency.BuildAugment(tc.encoder, qpMap, w, h, 2)
			if (err != nil) != tc.wantErr {
				t.Fatalf("BuildAugment error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				var unsupported *saliency.UnsupportedEncoderError
				if !errors.As(err, &unsupported) {
					t.Fatalf("error = %v, want an *UnsupportedEncoderError", err)
				}
				if !strings.Contains(err.Error(), "--saliency-fallback-plain") {
					t.Error("the message should name the opt-out flag")
				}
				if !strings.Contains(err.Error(), "libx264") {
					t.Error("the message should list the supported encoders")
				}
				return
			}
			if got.SidecarSuffix != tc.wantSuffix {
				t.Errorf("SidecarSuffix = %q, want %q", got.SidecarSuffix, tc.wantSuffix)
			}
			if tc.wantExtraParam != "" {
				if len(got.ExtraParams) == 0 || got.ExtraParams[0] != tc.wantExtraParam {
					t.Errorf("ExtraParams = %v, want it to start with %q",
						got.ExtraParams, tc.wantExtraParam)
				}
			}
			if tc.wantBodyPrefix != "" &&
				!strings.HasPrefix(got.SidecarBody, tc.wantBodyPrefix) {
				t.Errorf("SidecarBody starts %q, want prefix %q",
					firstLine(got.SidecarBody), tc.wantBodyPrefix)
			}
		})
	}
}

// firstLine returns s up to its first newline, for readable failures.
func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

// TestExtraParamsFor_matchesPythonGolden pins each codec's ROI argv channel.
func TestExtraParamsFor_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	tests := []struct {
		encoder string
		want    []string
	}{
		{"libx264", []string{"-x264-params", "qpfile=/tmp/q.txt"}},
		{"libaom-av1", []string{"-qpfile", "/tmp/q.txt"}},
		{"libsvtav1", []string{"-svtav1-params", "qp-file=/tmp/q.txt"}},
		{"libvvenc", []string{"-vvenc-params", "ROIFile=/tmp/q.txt"}},
		{"libx265", nil},
		{"h264_nvenc", nil},
	}
	for _, tc := range tests {
		t.Run(tc.encoder, func(t *testing.T) {
			t.Parallel()

			if got := saliency.ExtraParamsFor(tc.encoder, "/tmp/q.txt"); !slices.Equal(got, tc.want) {
				t.Errorf("ExtraParamsFor(%q) = %v, want %v", tc.encoder, got, tc.want)
			}
		})
	}
}

// TestWriteSidecar covers the write path and the argv-only no-op.
func TestWriteSidecar(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	path := filepath.Join(dir, "nested", "roi.txt")

	if err := saliency.WriteSidecar(
		saliency.Augment{SidecarBody: "0 I 0\n"}, path); err != nil {
		t.Fatalf("WriteSidecar: %v", err)
	}
	data, err := os.ReadFile(path) // #nosec G304 -- test-controlled path
	if err != nil {
		t.Fatalf("read sidecar: %v", err)
	}
	if string(data) != "0 I 0\n" {
		t.Errorf("sidecar body = %q", data)
	}

	noop := filepath.Join(dir, "unwritten.txt")
	if err := saliency.WriteSidecar(saliency.Augment{}, noop); err != nil {
		t.Fatalf("WriteSidecar no-op: %v", err)
	}
	if _, statErr := os.Stat(noop); statErr == nil {
		t.Error("an argv-only augment should not write a file")
	}
}

// TestPersistedSidecarPath mirrors the Python output.with_suffix convention.
func TestPersistedSidecarPath(t *testing.T) {
	t.Parallel()

	tests := []struct {
		output string
		suffix string
		want   string
	}{
		{"/out/enc.mp4", ".qpfile.txt", "/out/enc.qpfile.txt"},
		{"/out/enc.mkv", ".vvenc-roi.csv", "/out/enc.vvenc-roi.csv"},
		{"/out/enc", ".qpfile.txt", "/out/enc.qpfile.txt"},
	}
	for _, tc := range tests {
		t.Run(tc.output, func(t *testing.T) {
			t.Parallel()

			if got := saliency.PersistedSidecarPath(tc.output, tc.suffix); got != tc.want {
				t.Errorf("PersistedSidecarPath = %q, want %q", got, tc.want)
			}
		})
	}
}

// TestFallbackAllowed covers both opt-in channels (ADR-0546).
func TestFallbackAllowed(t *testing.T) {
	// Not parallel: mutates the process environment.
	t.Setenv("VMAFTUNE_SALIENCY_FALLBACK_OK", "")

	if saliency.FallbackAllowed(saliency.DefaultConfig()) {
		t.Error("fallback should be off by default")
	}
	cfg := saliency.DefaultConfig()
	cfg.AllowUnsupportedEncoderFallback = true
	if !saliency.FallbackAllowed(cfg) {
		t.Error("the config flag should enable fallback")
	}

	t.Setenv("VMAFTUNE_SALIENCY_FALLBACK_OK", "1")
	if !saliency.FallbackAllowed(saliency.DefaultConfig()) {
		t.Error("the env override should enable fallback")
	}
	t.Setenv("VMAFTUNE_SALIENCY_FALLBACK_OK", "0")
	if saliency.FallbackAllowed(saliency.DefaultConfig()) {
		t.Error("only the literal 1 should enable the env override")
	}
}

// TestConfigValidate covers the aggregator and alpha guards.
func TestConfigValidate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		agg     saliency.Aggregator
		alpha   float64
		wantErr bool
	}{
		{"defaults", saliency.AggMean, 0.6, false},
		{"ema at the upper bound", saliency.AggEMA, 1.0, false},
		{"alpha at zero", saliency.AggEMA, 0.0, true},
		{"alpha above one", saliency.AggEMA, 1.5, true},
		{"negative alpha", saliency.AggEMA, -0.1, true},
		{"unknown aggregator", "median", 0.6, true},
		{"empty aggregator", "", 0.6, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			cfg := saliency.Config{TemporalAggregator: tc.agg, EMAAlpha: tc.alpha}
			if err := cfg.Validate(); (err != nil) != tc.wantErr {
				t.Errorf("Validate error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestFrameCountAndReadFrame covers the raw-YUV reader.
func TestFrameCountAndReadFrame(t *testing.T) {
	t.Parallel()

	const w, h, n = 16, 16, 3
	dir := t.TempDir()
	path := filepath.Join(dir, "src.yuv")
	frameSize := saliency.FrameSizeBytes(w, h)
	buf := make([]byte, frameSize*n)
	// Give each frame a distinct luma so the reader's offset is verifiable.
	for f := 0; f < n; f++ {
		for i := 0; i < frameSize; i++ {
			buf[f*frameSize+i] = byte(10 * (f + 1))
		}
	}
	if err := os.WriteFile(path, buf, 0o600); err != nil {
		t.Fatalf("write yuv: %v", err)
	}

	count, err := saliency.FrameCount(path, w, h)
	if err != nil {
		t.Fatalf("FrameCount: %v", err)
	}
	if count != n {
		t.Errorf("FrameCount = %d, want %d", count, n)
	}

	for f := 0; f < n; f++ {
		frame, readErr := saliency.ReadFrame(path, f, w, h)
		if readErr != nil {
			t.Fatalf("ReadFrame(%d): %v", f, readErr)
		}
		if frame.Y[0] != byte(10*(f+1)) {
			t.Errorf("frame %d luma = %d, want %d", f, frame.Y[0], 10*(f+1))
		}
		if len(frame.Y) != w*h || len(frame.U) != w*h/4 || len(frame.V) != w*h/4 {
			t.Errorf("frame %d plane sizes = %d/%d/%d",
				f, len(frame.Y), len(frame.U), len(frame.V))
		}
	}

	if _, err := saliency.ReadFrame(path, n, w, h); err == nil {
		t.Error("reading past the end should fail")
	}
	if _, err := saliency.FrameCount(filepath.Join(dir, "missing.yuv"), w, h); err == nil {
		t.Error("FrameCount on a missing file should fail")
	}
}
