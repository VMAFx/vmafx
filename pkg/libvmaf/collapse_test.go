// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/collapse_test.go — tests of the collapse of the direct, stream
// and subprocess paths onto pkg/vmafx (#2434).
//
// The pooled score of a pair is computed four ways and compared by IEEE-754
// bits: the libvmaf C API (testdata/c_api_pooled_mean.c, compiled and run by
// the test), pkg/vmafx driven directly by the test, ScoreDirect, and the
// StreamScorer; the vmaf CLI run through Scorer.Run joins them at
// --precision=max (%.17g round-trips). Every test runs against the
// Meson-built library the CGO flags point at.
//
// Fixtures: the Netflix 576x324 pair (python/test/resource/yuv, untracked;
// VMAFX_YUV_REF and VMAFX_YUV_DIS override the paths as in the Rust binding
// tests) and the tracked 48-frame pair under testdata/. A missing fixture
// skips the test with the path named; nothing else is substituted.

//go:build cgo

package libvmaf

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/vmaftest"
	"github.com/VMAFx/vmafx/pkg/vmafx"
)

const (
	bitExactModel = "vmaf_v1.0.16_3d0h"
	frame8bit420  = 576 * 324 * 3 / 2
	// maxFixtureFrames bounds the fixture loop; every fixture holds far fewer.
	maxFixtureFrames = 4096
)

// pairFixture is a raw 8-bit 4:2:0 YUV pair.
type pairFixture struct {
	name     string
	ref, dis string
	w, h     int
}

func requireFiles(t *testing.T, paths ...string) {
	t.Helper()
	for _, p := range paths {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s): %v", p, err)
		}
	}
}

// netflixFixture is the Netflix 576x324 normal pair.
func netflixFixture(t *testing.T) pairFixture {
	t.Helper()
	yuv := filepath.Join(RepoRoot(), "python", "test", "resource", "yuv")
	fx := pairFixture{
		name: "netflix",
		ref:  filepath.Join(yuv, "src01_hrc00_576x324.yuv"),
		dis:  filepath.Join(yuv, "src01_hrc01_576x324.yuv"),
		w:    576, h: 324,
	}
	if v := os.Getenv("VMAFX_YUV_REF"); v != "" {
		fx.ref = v
	}
	if v := os.Getenv("VMAFX_YUV_DIS"); v != "" {
		fx.dis = v
	}
	requireFiles(t, fx.ref, fx.dis)
	return fx
}

// trackedFixture is the 48-frame pair tracked in testdata/.
func trackedFixture(t *testing.T) pairFixture {
	t.Helper()
	fx := pairFixture{
		name: "testdata48",
		ref:  filepath.Join(RepoRoot(), "testdata", "ref_576x324_48f.yuv"),
		dis:  filepath.Join(RepoRoot(), "testdata", "dis_576x324_48f.yuv"),
		w:    576, h: 324,
	}
	requireFiles(t, fx.ref, fx.dis)
	return fx
}

// bitExactModelPath is vmaf_v1.0.16_3d0h from model/; a missing model skips.
func bitExactModelPath(t *testing.T) string {
	t.Helper()
	p := filepath.Join(RepoRoot(), "model", "vmaf_v1.0.16", bitExactModel+".json")
	requireFiles(t, p)
	return p
}

// firstFrames writes the first n frames of src to a temp file, widening every
// sample to 16 bits (value << shift, little endian) when shift is positive.
func firstFrames(t *testing.T, src string, n, shift int) string {
	t.Helper()
	raw, err := os.ReadFile(src)
	if err != nil {
		t.Fatalf("read %s: %v", src, err)
	}
	if len(raw) < n*frame8bit420 {
		t.Fatalf("%s holds %d bytes, want at least %d", src, len(raw), n*frame8bit420)
	}
	raw = raw[:n*frame8bit420]
	if shift > 0 {
		wide := make([]byte, 0, 2*len(raw))
		for _, b := range raw {
			wide = binary.LittleEndian.AppendUint16(wide, uint16(b)<<shift)
		}
		raw = wide
	}
	dst := filepath.Join(t.TempDir(), "frames.yuv")
	if err := os.WriteFile(dst, raw, 0o600); err != nil {
		t.Fatalf("write %s: %v", dst, err)
	}
	return dst
}

// buildCAPIHarness compiles testdata/c_api_pooled_mean.c against the library
// the CGO flags select and returns the executable path.
func buildCAPIHarness(t *testing.T) string {
	t.Helper()
	cc, err := exec.LookPath("cc")
	if err != nil {
		t.Skipf("no C compiler on PATH: %v", err)
	}
	ldflags := strings.Fields(os.Getenv("CGO_LDFLAGS"))
	if len(ldflags) == 0 {
		t.Skip("CGO_LDFLAGS is unset: cannot link the C API harness")
	}
	exe := filepath.Join(t.TempDir(), "c_api_pooled_mean")
	args := []string{
		"-O2", "-Wall", "-Werror",
		"-I", filepath.Join(RepoRoot(), "core", "include"),
		filepath.Join("testdata", "c_api_pooled_mean.c"),
		"-o", exe,
	}
	args = append(args, ldflags...)
	for _, f := range ldflags {
		if dir, ok := strings.CutPrefix(f, "-L"); ok {
			args = append(args, "-Wl,-rpath,"+dir)
		}
	}
	if out, err := exec.Command(cc, args...).CombinedOutput(); err != nil { // #nosec G204 -- fixed test harness
		t.Fatalf("compile the C API harness: %v\n%s", err, out)
	}
	return exe
}

// cAPIPooled scores the pair through the libvmaf C API and returns the pooled
// value of the method as printed in hexadecimal float form.
func cAPIPooled(t *testing.T, exe string, ref, dis, model string, w, h, bpc int, pool PoolMethod) float64 {
	t.Helper()
	cmd := exec.Command(exe, ref, dis, model, // #nosec G204 -- the harness this test built
		strconv.Itoa(w), strconv.Itoa(h), strconv.Itoa(int(PixFmtYUV420P)),
		strconv.Itoa(bpc), strconv.Itoa(int(pool)))
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("C API harness: %v\n%s", err, stderr.String())
	}
	v, err := strconv.ParseFloat(strings.TrimSpace(string(out)), 64)
	if err != nil {
		t.Fatalf("C API harness printed %q: %v", out, err)
	}
	return v
}

// readFrameRaw fills the planes of frame from r, row by row, and reports
// whether a frame was read (false at a clean end-of-file). It is written
// against pkg/vmafx alone, independent of fillFrame, so that the bit-exactness
// test has an oracle that does not share the code under test.
func readFrameRaw(t *testing.T, frame *vmafx.Frame, r io.Reader) bool {
	t.Helper()
	planes, err := frame.Planes()
	if err != nil {
		t.Fatalf("Planes: %v", err)
	}
	sample := 1
	if planes.Bpc > 8 {
		sample = 2
	}
	for p := uint32(0); p < planes.NPlanes; p++ {
		buf, err := frame.PlaneBytes(p)
		if err != nil {
			t.Fatalf("PlaneBytes(%d): %v", p, err)
		}
		rowBytes := int(planes.W[p]) * sample
		for row := 0; row < int(planes.H[p]); row++ {
			off := row * int(planes.Stride[p])
			if _, err := io.ReadFull(r, buf[off:off+rowBytes]); err != nil {
				if p == 0 && row == 0 && errors.Is(err, io.EOF) {
					return false
				}
				t.Fatalf("read plane %d row %d: %v", p, row, err)
			}
		}
	}
	return true
}

// vmafxPooled scores the pair by driving pkg/vmafx directly.
func vmafxPooled(t *testing.T, ref, dis, model string, w, h, bpc int, pool PoolMethod) float64 {
	t.Helper()
	ctx, err := vmafx.New(&vmafx.ContextConfig{LogLevel: vmafx.LogLevelWarning, NSubsample: 1})
	if err != nil {
		t.Fatalf("vmafx.New: %v", err)
	}
	defer func() {
		if err := ctx.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
	}()
	m, err := vmafx.LoadModelFile(&vmafx.ModelConfig{Name: "vmafx_direct"}, model)
	if err != nil {
		t.Fatalf("LoadModelFile: %v", err)
	}
	defer m.Close()
	if err := ctx.UseModel(m); err != nil {
		t.Fatalf("UseModel: %v", err)
	}
	rf, err := os.Open(ref)
	if err != nil {
		t.Fatalf("open ref: %v", err)
	}
	defer func() { _ = rf.Close() }()
	df, err := os.Open(dis)
	if err != nil {
		t.Fatalf("open dis: %v", err)
	}
	defer func() { _ = df.Close() }()
	desc := vmafx.FrameDesc{PixFmt: uint32(PixFmtYUV420P), Bpc: uint32(bpc), W: uint32(w), H: uint32(h)}
	n := 0
	for ; n < maxFixtureFrames; n++ {
		rframe, err := vmafx.NewHostFrame(desc)
		if err != nil {
			t.Fatalf("NewHostFrame(ref): %v", err)
		}
		dframe, err := vmafx.NewHostFrame(desc)
		if err != nil {
			t.Fatalf("NewHostFrame(dis): %v", err)
		}
		gotRef, gotDis := readFrameRaw(t, rframe, rf), readFrameRaw(t, dframe, df)
		if gotRef != gotDis {
			t.Fatalf("ref and dis differ in length at frame %d", n)
		}
		if !gotRef {
			rframe.Close()
			dframe.Close()
			break
		}
		if err := ctx.Submit(rframe, dframe, uint64(n)); err != nil {
			t.Fatalf("Submit(%d): %v", n, err)
		}
	}
	if n == maxFixtureFrames {
		t.Fatalf("fixture holds more than %d frames", maxFixtureFrames)
	}
	if err := ctx.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	pooledPool, err := pool.toPool()
	if err != nil {
		t.Fatalf("toPool: %v", err)
	}
	score, err := ctx.ScorePooled(m, pooledPool, 0, uint64(n-1))
	if err != nil {
		t.Fatalf("ScorePooled: %v", err)
	}
	return score.Value
}

func directPooled(t *testing.T, ref, dis, model string, w, h, bpc int, pool PoolMethod) *ScoreDirectResult {
	t.Helper()
	res, err := ScoreDirect(context.Background(), ScoreDirectRequest{
		Ref: ref, Dis: dis, ModelPath: model, Width: w, Height: h,
		PixFmt: PixFmtYUV420P, BitDepth: bpc, PoolMethod: pool,
	})
	if err != nil {
		t.Fatalf("ScoreDirect: %v", err)
	}
	return res
}

func requireSameBits(t *testing.T, what string, got, want float64) {
	t.Helper()
	if math.Float64bits(got) != math.Float64bits(want) {
		t.Fatalf("%s: got %x (%v), want %x (%v)", what, got, got, want, want)
	}
}

// TestPooledMeanBitExactAcrossBindings is the acceptance check of #2434: the
// pooled mean of vmaf_v1.0.16_3d0h through the direct path equals the value
// through pkg/vmafx and the libvmaf C API bit for bit, on the Netflix pair and
// on the tracked 48-frame pair.
func TestPooledMeanBitExactAcrossBindings(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fixtures := map[string]func(*testing.T) pairFixture{
		"netflix": netflixFixture, "testdata48": trackedFixture,
	}
	for name, load := range fixtures {
		t.Run(name, func(t *testing.T) {
			fx := load(t)
			want := cAPIPooled(t, exe, fx.ref, fx.dis, model, fx.w, fx.h, 8, PoolMethodMean)
			requireSameBits(t, "pkg/vmafx vs C API", vmafxPooled(t, fx.ref, fx.dis, model, fx.w, fx.h, 8, PoolMethodMean), want)
			res := directPooled(t, fx.ref, fx.dis, model, fx.w, fx.h, 8, PoolMethodMean)
			requireSameBits(t, "ScoreDirect vs C API", res.VMAF, want)
			t.Logf("%s pooled mean %x (%.17g), %d frames", fx.name, want, want, res.FrameCount)
		})
	}
}

// TestPooledMeanBitExactEveryPoolMethod covers the boundary of the pooling
// enum: every method of PoolMethod agrees between the C API and ScoreDirect.
func TestPooledMeanBitExactEveryPoolMethod(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	for pool := PoolMethodMin; pool <= PoolMethodPerc20; pool++ {
		t.Run(pool.String(), func(t *testing.T) {
			want := cAPIPooled(t, exe, fx.ref, fx.dis, model, fx.w, fx.h, 8, pool)
			got := directPooled(t, fx.ref, fx.dis, model, fx.w, fx.h, 8, pool)
			requireSameBits(t, "ScoreDirect vs C API", got.VMAF, want)
			requireSameBits(t, "pkg/vmafx vs C API",
				vmafxPooled(t, fx.ref, fx.dis, model, fx.w, fx.h, 8, pool), want)
		})
	}
}

// TestPooledMeanBitExactBoundaryGeometries covers a single frame and the
// two-byte sample path (10-bit samples widened from the 8-bit fixture).
func TestPooledMeanBitExactBoundaryGeometries(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	cases := []struct {
		name   string
		frames int
		shift  int
		bpc    int
	}{
		{"single_frame_8bit", 1, 0, 8},
		{"twelve_frames_10bit", 12, 2, 10},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			ref := firstFrames(t, fx.ref, tc.frames, tc.shift)
			dis := firstFrames(t, fx.dis, tc.frames, tc.shift)
			want := cAPIPooled(t, exe, ref, dis, model, fx.w, fx.h, tc.bpc, PoolMethodMean)
			got := directPooled(t, ref, dis, model, fx.w, fx.h, tc.bpc, PoolMethodMean)
			if got.FrameCount != tc.frames {
				t.Fatalf("FrameCount = %d, want %d", got.FrameCount, tc.frames)
			}
			requireSameBits(t, "ScoreDirect vs C API", got.VMAF, want)
			requireSameBits(t, "pkg/vmafx vs C API",
				vmafxPooled(t, ref, dis, model, fx.w, fx.h, tc.bpc, PoolMethodMean), want)
		})
	}
}

// TestStreamScorerBitExactWithCAPI pushes the tracked pair frame by frame and
// compares the pooled score with the C API by bits (stream path).
func TestStreamScorerBitExactWithCAPI(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	want := cAPIPooled(t, exe, fx.ref, fx.dis, model, fx.w, fx.h, 8, PoolMethodMean)

	s, err := NewStreamScorer(StreamConfig{
		Width: fx.w, Height: fx.h, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer func() {
		if err := s.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
	}()
	if n := pushAll(t, s, fx.ref, fx.dis); n != 48 {
		t.Fatalf("pushed %d frames, want 48", n)
	}
	res, err := s.Finish(context.Background())
	if err != nil {
		t.Fatalf("Finish: %v", err)
	}
	requireSameBits(t, "StreamScorer vs C API", res.Score, want)
	record, digest, err := s.Provenance()
	if err != nil || len(record) == 0 || len(digest) != 64 {
		t.Fatalf("Provenance = (%d bytes, %q, %v), want a record and a SHA-256", len(record), digest, err)
	}
}

// TestStreamScorerBoundaryOneFrame is the boundary of the stream path: one
// pushed frame scores and equals ScoreDirect over the same single frame.
func TestStreamScorerBoundaryOneFrame(t *testing.T) {
	model := bitExactModelPath(t)
	fx := trackedFixture(t)
	ref := firstFrames(t, fx.ref, 1, 0)
	dis := firstFrames(t, fx.dis, 1, 0)
	s, err := NewStreamScorer(StreamConfig{
		Width: fx.w, Height: fx.h, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer func() { _ = s.Close() }()
	if n := pushAll(t, s, ref, dis); n != 1 {
		t.Fatalf("pushed %d frames, want 1", n)
	}
	res, err := s.Finish(context.Background())
	if err != nil {
		t.Fatalf("Finish: %v", err)
	}
	want := directPooled(t, ref, dis, model, fx.w, fx.h, 8, PoolMethodMean)
	requireSameBits(t, "StreamScorer vs ScoreDirect", res.Score, want.VMAF)
	if res.FramesProcessed != 1 || len(res.Frames) != 1 {
		t.Fatalf("FramesProcessed=%d len(Frames)=%d, want 1/1", res.FramesProcessed, len(res.Frames))
	}
	requireSameBits(t, "frame 0 score vs pooled mean", res.Frames[0].Score, res.Score)
}

// TestScorerRunBitExactWithCAPI joins the subprocess path: the vmaf CLI run
// by Scorer.Run at --precision=max reports the pooled mean the C API computes,
// to the bit.
func TestScorerRunBitExactWithCAPI(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	want := cAPIPooled(t, exe, fx.ref, fx.dis, model, fx.w, fx.h, 8, PoolMethodMean)

	scorer, err := New(vmaftest.Binary(t), filepath.Join(RepoRoot(), "model"))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	res, err := scorer.Run(context.Background(), Request{
		Reference: fx.ref, Distorted: fx.dis, Model: bitExactModel,
		Args: []string{
			"--precision=max", "--width", "576", "--height", "324", "--pixel_format", "420",
			"--bitdepth", "8", "--threads", "1", "--backend", "cpu",
		},
	})
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	requireSameBits(t, "Scorer.Run (vmaf CLI) vs C API", res.Score, want)
}

// TestScoreDirectCorruptInputsAreTyped covers the negative cases of the direct
// path against the real library: a model file that is not a model, and
// frame files whose length is not a whole number of frames.
func TestScoreDirectCorruptInputsAreTyped(t *testing.T) {
	model := bitExactModelPath(t)
	fx := trackedFixture(t)
	bad := filepath.Join(t.TempDir(), "bad_model.json")
	if err := os.WriteFile(bad, []byte(`{"not": "a model"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	req := ScoreDirectRequest{
		Ref: fx.ref, Dis: fx.dis, ModelPath: bad, Width: fx.w, Height: fx.h,
		PixFmt: PixFmtYUV420P, BitDepth: 8,
	}
	if _, err := ScoreDirect(context.Background(), req); !errors.Is(err, ErrInvalidArgument) || !errors.Is(err, os.ErrInvalid) {
		t.Errorf("corrupt model: %v, want ErrInvalidArgument and os.ErrInvalid", err)
	}
	var vErr *vmafx.Error
	if _, err := ScoreDirect(context.Background(), req); !errors.As(err, &vErr) {
		t.Errorf("corrupt model: %v does not carry the *vmafx.Error", err)
	}
	if err := ValidateModel(bad); !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("ValidateModel(corrupt) = %v, want ErrInvalidArgument", err)
	}
	if err := ValidateModel(model); err != nil {
		t.Errorf("ValidateModel(real model) = %v, want nil", err)
	}

	whole := firstFrames(t, fx.ref, 2, 0)
	raw, err := os.ReadFile(whole)
	if err != nil {
		t.Fatal(err)
	}
	half := filepath.Join(t.TempDir(), "half.yuv")
	if err := os.WriteFile(half, raw[:frame8bit420+frame8bit420/2], 0o600); err != nil {
		t.Fatal(err)
	}
	req.ModelPath = model
	for name, pair := range map[string][2]string{
		"ref_truncated":  {half, whole},
		"dis_truncated":  {whole, half},
		"both_truncated": {half, half},
	} {
		req.Ref, req.Dis = pair[0], pair[1]
		if _, err := ScoreDirect(context.Background(), req); !errors.Is(err, ErrPictureRead) {
			t.Errorf("%s: %v, want ErrPictureRead", name, err)
		}
	}
	req.Ref, req.Dis, req.PoolMethod = whole, whole, PoolMethod(99)
	if _, err := ScoreDirect(context.Background(), req); !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("pool method 99: %v, want ErrInvalidArgument", err)
	}
}

// TestFillFrameFromBytes covers the plane fill the stream path uses: exact
// length succeeds, a source one byte short or one byte long is ErrPictureRead.
func TestFillFrameFromBytes(t *testing.T) {
	desc := vmafx.FrameDesc{PixFmt: uint32(PixFmtYUV420P), Bpc: 8, W: 16, H: 8}
	size := frameBytes(16, 8, PixFmtYUV420P, 8)
	cases := []struct {
		name string
		n    int
		ok   bool
	}{
		{"exact", size, true},
		{"one_short", size - 1, false},
		{"one_long", size + 1, false},
		{"empty", 0, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			frame, err := vmafx.NewHostFrame(desc)
			if err != nil {
				t.Fatalf("NewHostFrame: %v", err)
			}
			defer frame.Close()
			err = fillFrameFromBytes(frame, bytes.Repeat([]byte{7}, tc.n))
			if tc.ok != (err == nil) {
				t.Fatalf("fillFrameFromBytes(%d bytes) = %v, ok want %v", tc.n, err, tc.ok)
			}
			if !tc.ok && !errors.Is(err, ErrPictureRead) {
				t.Fatalf("error %v is not ErrPictureRead", err)
			}
		})
	}
}

// TestMapVmafxErrorKeepsTheErrnoContract pins the migrated errno contract of
// errors.go: every status the engine reports for the four historic errnos maps
// to the same sentinel as before, with os.ErrInvalid / os.ErrNotExist intact,
// and the *vmafx.Error stays in the chain.
func TestMapVmafxErrorKeepsTheErrnoContract(t *testing.T) {
	cases := []struct {
		name    string
		err     *vmafx.Error
		want    error
		wantStd error
	}{
		{"invalid", &vmafx.Error{Status: vmafx.StatusEInvalid, Errno: -22}, ErrInvalidArgument, os.ErrInvalid},
		{"invalid_without_errno", &vmafx.Error{Status: vmafx.StatusEInvalid}, ErrInvalidArgument, os.ErrInvalid},
		{"invalid_placeholder_errno", &vmafx.Error{Status: vmafx.StatusEInvalid, Errno: -1}, ErrInvalidArgument, os.ErrInvalid},
		{"io_placeholder_errno", &vmafx.Error{Status: vmafx.StatusEIO, Errno: -1}, ErrPictureRead, nil},
		{"notfound", &vmafx.Error{Status: vmafx.StatusENotFound, Errno: -2}, ErrModelNotFound, os.ErrNotExist},
		{"notfound_without_errno", &vmafx.Error{Status: vmafx.StatusENotFound}, ErrModelNotFound, os.ErrNotExist},
		{"nomem", &vmafx.Error{Status: vmafx.StatusENoMem, Errno: -12}, ErrOutOfMemory, nil},
		{"io", &vmafx.Error{Status: vmafx.StatusEIO, Errno: -5}, ErrPictureRead, nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := mapVmafxError("vmafx_test", tc.err)
			if !errors.Is(got, tc.want) {
				t.Fatalf("%v is not %v", got, tc.want)
			}
			if tc.wantStd != nil && !errors.Is(got, tc.wantStd) {
				t.Fatalf("%v is not %v", got, tc.wantStd)
			}
			var vErr *vmafx.Error
			if !errors.As(got, &vErr) || vErr != tc.err {
				t.Fatalf("%v lost the *vmafx.Error", got)
			}
		})
	}
	if mapVmafxError("vmafx_test", nil) != nil {
		t.Fatal("a nil error must stay nil")
	}
	if got := mapCloseError(nil); got != nil {
		t.Fatalf("mapCloseError(nil) = %v", got)
	}
	if got := mapCloseError(&vmafx.Error{Status: vmafx.StatusENoMem, Errno: -12}); !errors.Is(got, ErrOutOfMemory) {
		t.Fatalf("mapCloseError(ENOMEM) = %v, want ErrOutOfMemory", got)
	}
}

// TestMapVmafxErrorBoundaries covers the statuses with no historic errno: a
// device status must not be read as an errno, an engine errno outside the four
// keeps the numeric fallback, and a foreign error is wrapped as is.
func TestMapVmafxErrorBoundaries(t *testing.T) {
	device := &vmafx.Error{Status: vmafx.StatusEDevice, Errno: -22}
	got := mapVmafxError("vmafx_test", device)
	if errors.Is(got, ErrInvalidArgument) {
		t.Fatalf("a StatusEDevice with backend code -22 must not map to ErrInvalidArgument: %v", got)
	}
	if !errors.Is(got, vmafx.ErrDevice) {
		t.Fatalf("%v does not wrap vmafx.ErrDevice", got)
	}
	engine := &vmafx.Error{Status: vmafx.StatusEInternal, Errno: -38}
	got = mapVmafxError("vmafx_test", engine)
	if !strings.Contains(got.Error(), "returned -38") || !errors.Is(got, vmafx.ErrInternal) {
		t.Fatalf("engine errno fallback = %v", got)
	}
	foreign := errors.New("not a vmafx error")
	if got := mapVmafxError("vmafx_test", foreign); !errors.Is(got, foreign) {
		t.Fatalf("a foreign error must stay in the chain: %v", got)
	}
}

// TestLoadScoringModelErrors covers the model loader of both in-process paths
// against the real library: a missing file is ErrModelNotFound (os.ErrNotExist)
// although vmafx reports it as a file error, a directory and a file that is not
// a model are not, and a real model loads.
func TestLoadScoringModelErrors(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "absent.json")
	_, err := loadScoringModel(missing, "m")
	if !errors.Is(err, ErrModelNotFound) || !errors.Is(err, os.ErrNotExist) || !errors.Is(err, vmafx.ErrIO) {
		t.Errorf("missing model: %v, want ErrModelNotFound, os.ErrNotExist and the vmafx error", err)
	}
	if _, err := loadScoringModel(t.TempDir(), "m"); err == nil || errors.Is(err, ErrModelNotFound) {
		t.Errorf("directory as model: %v, want an error that is not ErrModelNotFound", err)
	}
	notModel := filepath.Join(t.TempDir(), "not_a_model.json")
	if err := os.WriteFile(notModel, []byte(`[]`), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadScoringModel(notModel, "m"); !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("file that is not a model: %v, want ErrInvalidArgument", err)
	}
	model, err := loadScoringModel(bitExactModelPath(t), "m")
	if err != nil {
		t.Fatalf("real model: %v", err)
	}
	model.Close()
}

// TestRequireGeometryAndModel pins the validation both in-process paths share:
// the boundaries accepted and the first violation reported, with the same
// error from ScoreDirect and NewStreamScorer.
func TestRequireGeometryAndModel(t *testing.T) {
	cases := []struct {
		name     string
		w, h, bd int
		pf       PixelFormat
		ok       bool
	}{
		{"smallest", 1, 1, 8, PixFmtYUV420P, true},
		{"ten_bit_444", 64, 64, 10, PixFmtYUV444P, true},
		{"twelve_bit", 64, 64, 12, PixFmtYUV422P, true},
		{"zero_width", 0, 8, 8, PixFmtYUV420P, false},
		{"negative_height", 8, -1, 8, PixFmtYUV420P, false},
		{"no_pixel_format", 8, 8, 8, 0, false},
		{"bit_depth_9", 8, 8, 9, PixFmtYUV420P, false},
		{"bit_depth_13", 8, 8, 13, PixFmtYUV420P, false},
		{"bit_depth_0", 8, 8, 0, PixFmtYUV420P, false},
	}
	model := bitExactModelPath(t)
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := requireGeometry("op", tc.w, tc.h, tc.pf, tc.bd)
			if tc.ok != (err == nil) {
				t.Fatalf("requireGeometry = %v, ok want %v", err, tc.ok)
			}
			if tc.ok {
				return
			}
			if !errors.Is(err, ErrInvalidArgument) || !errors.Is(err, os.ErrInvalid) {
				t.Fatalf("%v is not ErrInvalidArgument / os.ErrInvalid", err)
			}
			_, streamErr := NewStreamScorer(StreamConfig{
				Width: tc.w, Height: tc.h, PixFmt: tc.pf, BitDepth: tc.bd, ModelPath: model,
			})
			_, directErr := ScoreDirect(context.Background(), ScoreDirectRequest{
				Ref: "a", Dis: "b", ModelPath: model, Width: tc.w, Height: tc.h, PixFmt: tc.pf, BitDepth: tc.bd,
			})
			if streamErr == nil || directErr == nil {
				t.Fatalf("both paths must reject: stream=%v direct=%v", streamErr, directErr)
			}
			if strings.TrimPrefix(streamErr.Error(), "StreamScorer") != strings.TrimPrefix(directErr.Error(), "ScoreDirect") {
				t.Fatalf("paths disagree: stream=%q direct=%q", streamErr, directErr)
			}
		})
	}
	if err := requireModelPath("op", ""); !errors.Is(err, ErrInvalidArgument) {
		t.Fatalf("requireModelPath(\"\") = %v", err)
	}
	if err := requireModelPath("op", model); err != nil {
		t.Fatalf("requireModelPath(model) = %v", err)
	}
	missing := filepath.Join(t.TempDir(), "absent.json")
	if err := requireModelExists("op", missing); !errors.Is(err, ErrModelNotFound) || !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("requireModelExists(missing) = %v", err)
	}
	if err := requireModelExists("op", model); err != nil {
		t.Fatalf("requireModelExists(model) = %v", err)
	}
}

// TestSubprocessSharedHelpers covers what the file-pair and the pipe entry
// points of the subprocess path share: the failure text, the command and the
// run set-up.
func TestSubprocessSharedHelpers(t *testing.T) {
	t.Run("cliFailure_genuine_failure", func(t *testing.T) {
		stderr := bytes.NewBufferString("boom")
		err := cliFailure(context.Background(), errors.New("exit status 1"), stderr)
		if !strings.Contains(err.Error(), "vmaf binary failed") || !strings.Contains(err.Error(), "boom") {
			t.Fatalf("cliFailure = %v", err)
		}
		if errors.Is(err, context.Canceled) {
			t.Fatalf("a genuine failure must not report a cancellation: %v", err)
		}
	})
	t.Run("cliFailure_cancelled", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		err := cliFailure(ctx, errors.New("signal: killed"), &bytes.Buffer{})
		if !errors.Is(err, context.Canceled) || !strings.Contains(err.Error(), "subprocess cancelled") {
			t.Fatalf("cliFailure = %v", err)
		}
	})
	t.Run("prepareRun_resolves_and_releases", func(t *testing.T) {
		dir := t.TempDir()
		writeModel(t, dir, "m")
		s := &Scorer{binaryPath: "unused", modelDir: dir}
		run, err := s.prepareRun(nil, "Run", "m") //nolint:staticcheck // a nil context is documented as Background
		if err != nil {
			t.Fatalf("prepareRun: %v", err)
		}
		if run.modelPath != filepath.Join(dir, "m.json") {
			t.Fatalf("modelPath = %q", run.modelPath)
		}
		if _, err := os.Stat(run.out); err != nil {
			t.Fatalf("report file not created: %v", err)
		}
		run.release()
		if _, err := os.Stat(run.out); !errors.Is(err, os.ErrNotExist) {
			t.Fatalf("report file survives release: %v", err)
		}
		if run.ctx.Err() == nil {
			t.Fatal("release must cancel the run context")
		}
	})
	t.Run("prepareRun_model_not_found", func(t *testing.T) {
		s := &Scorer{binaryPath: "unused", modelDir: t.TempDir()}
		if _, err := s.prepareRun(context.Background(), "Run", "absent"); err == nil {
			t.Fatal("prepareRun must fail for an unknown model")
		}
	})
	t.Run("prepareRun_cancelled_names_the_operation", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		s := &Scorer{binaryPath: "unused"}
		_, err := s.prepareRun(ctx, "Score", "")
		if !errors.Is(err, context.Canceled) || !strings.Contains(err.Error(), "before Score") {
			t.Fatalf("prepareRun = %v", err)
		}
	})
	t.Run("newCLICommand_bounds_the_wait", func(t *testing.T) {
		s := &Scorer{binaryPath: "/bin/true"}
		cmd, stderr := s.newCLICommand(context.Background(), []string{"x"})
		if cmd.WaitDelay <= 0 || cmd.Stderr == nil || stderr == nil || cmd.Args[0] != "/bin/true" {
			t.Fatalf("newCLICommand = %+v", cmd)
		}
	})
}

// TestEngineFillFrameReadsEveryRowOnce plants a stride-aware check: a frame
// filled from a counting pattern reads back byte for byte through PlaneBytes,
// so a fill that ignored the stride would shift the rows.
func TestEngineFillFrameReadsEveryRowOnce(t *testing.T) {
	desc := vmafx.FrameDesc{PixFmt: uint32(PixFmtYUV420P), Bpc: 8, W: 18, H: 6}
	size := frameBytes(18, 6, PixFmtYUV420P, 8)
	src := make([]byte, size)
	for i := range src {
		src[i] = byte(i)
	}
	frame, err := vmafx.NewHostFrame(desc)
	if err != nil {
		t.Fatalf("NewHostFrame: %v", err)
	}
	defer frame.Close()
	if err := fillFrameFromBytes(frame, src); err != nil {
		t.Fatalf("fillFrameFromBytes: %v", err)
	}
	planes, err := frame.Planes()
	if err != nil {
		t.Fatalf("Planes: %v", err)
	}
	var back []byte
	for p := uint32(0); p < planes.NPlanes; p++ {
		buf, err := frame.PlaneBytes(p)
		if err != nil {
			t.Fatalf("PlaneBytes(%d): %v", p, err)
		}
		for row := 0; row < int(planes.H[p]); row++ {
			off := row * int(planes.Stride[p])
			back = append(back, buf[off:off+int(planes.W[p])]...)
		}
	}
	if !bytes.Equal(back, src) {
		t.Fatal("planes read back differ from the source")
	}
}
