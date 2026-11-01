// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/collapse_fixtures_test.go — fixtures and scoring helpers the
// collapse tests (#2434) share: the YUV pairs, the bit-exact model, the C API
// harness and the pooled score through each path.

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

// scoringCase is one scoring call the bit-exactness tests repeat through every
// path: the pair, the model, the geometry, the bit depth and the pool method.
// Every case is 4:2:0.
type scoringCase struct {
	ref, dis, model string
	w, h, bpc       int
	pool            PoolMethod
}

// scoring is the scoring case of the fixture at its own geometry.
func (fx pairFixture) scoring(model string, bpc int, pool PoolMethod) scoringCase {
	return scoringCase{ref: fx.ref, dis: fx.dis, model: model, w: fx.w, h: fx.h, bpc: bpc, pool: pool}
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
func cAPIPooled(t *testing.T, exe string, sc scoringCase) float64 {
	t.Helper()
	// The harness reads the pair from descriptors 3 and 4 and never opens a
	// path itself (ADR-1526 passes inputs the same way).
	inputs := make([]*os.File, 0, 2)
	for _, path := range []string{sc.ref, sc.dis} {
		f, err := os.Open(path)
		if err != nil {
			t.Fatalf("open %s: %v", path, err)
		}
		t.Cleanup(func() { _ = f.Close() })
		inputs = append(inputs, f)
	}
	cmd := exec.Command(exe, sc.model, // #nosec G204 -- the harness this test built
		strconv.Itoa(sc.w), strconv.Itoa(sc.h), strconv.Itoa(int(PixFmtYUV420P)),
		strconv.Itoa(sc.bpc), strconv.Itoa(int(sc.pool)))
	cmd.ExtraFiles = inputs
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

// submitFixtureFrames submits every frame pair of the two files and returns the
// frame count; a fixture over the bound fails the test.
func submitFixtureFrames(t *testing.T, ctx *vmafx.Context, rf, df *os.File, desc vmafx.FrameDesc) int {
	t.Helper()
	for n := range maxFixtureFrames {
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
			return n
		}
		if err := ctx.Submit(rframe, dframe, uint64(n)); err != nil {
			t.Fatalf("Submit(%d): %v", n, err)
		}
	}
	t.Fatalf("fixture holds more than %d frames", maxFixtureFrames)
	return 0
}

// vmafxPooled scores the pair by driving pkg/vmafx directly.
func vmafxPooled(t *testing.T, sc scoringCase) float64 {
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
	m, err := vmafx.LoadModelFile(&vmafx.ModelConfig{Name: "vmafx_direct"}, sc.model)
	if err != nil {
		t.Fatalf("LoadModelFile: %v", err)
	}
	defer m.Close()
	if err := ctx.UseModel(m); err != nil {
		t.Fatalf("UseModel: %v", err)
	}
	rf, err := os.Open(sc.ref)
	if err != nil {
		t.Fatalf("open ref: %v", err)
	}
	defer func() { _ = rf.Close() }()
	df, err := os.Open(sc.dis)
	if err != nil {
		t.Fatalf("open dis: %v", err)
	}
	defer func() { _ = df.Close() }()
	desc := vmafx.FrameDesc{PixFmt: uint32(PixFmtYUV420P), Bpc: uint32(sc.bpc), W: uint32(sc.w), H: uint32(sc.h)}
	n := submitFixtureFrames(t, ctx, rf, df, desc)
	if err := ctx.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	pooledPool, err := sc.pool.toPool()
	if err != nil {
		t.Fatalf("toPool: %v", err)
	}
	score, err := ctx.ScorePooled(m, pooledPool, 0, uint64(n-1))
	if err != nil {
		t.Fatalf("ScorePooled: %v", err)
	}
	return score.Value
}

func directPooled(t *testing.T, sc scoringCase) *ScoreDirectResult {
	t.Helper()
	res, err := ScoreDirect(context.Background(), ScoreDirectRequest{
		Ref: sc.ref, Dis: sc.dis, ModelPath: sc.model, Width: sc.w, Height: sc.h,
		PixFmt: PixFmtYUV420P, BitDepth: sc.bpc, PoolMethod: sc.pool,
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
