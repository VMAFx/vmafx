// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package vmafx

import (
	"encoding/json"
	"errors"
	"io"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// Tests of the model, frame and scoring wrappers (#2434) against the
// Meson-built library the CGO flags select. The model is vmaf_v1.0.16_3d0h
// from model/ and the frames are the first frames of the tracked 576x324
// pair under testdata/; a missing file skips the test with its path named.

const (
	testWidth  = 576
	testHeight = 324
)

func repoFile(t *testing.T, parts ...string) string {
	t.Helper()
	p := filepath.Join(append([]string{"..", ".."}, parts...)...)
	if _, err := os.Stat(p); err != nil {
		t.Skipf("fixture missing (%s): %v", p, err)
	}
	return p
}

func testModelPath(t *testing.T) string {
	t.Helper()
	return repoFile(t, "model", "vmaf_v1.0.16", "vmaf_v1.0.16_3d0h.json")
}

func testDesc() FrameDesc {
	return FrameDesc{PixFmt: 1, Bpc: 8, W: testWidth, H: testHeight}
}

// loadModel loads the test model under the name "gen" and closes it with the
// test.
func loadModel(t *testing.T) *Model {
	t.Helper()
	model, err := LoadModelFile(&ModelConfig{Name: "gen"}, testModelPath(t))
	if err != nil {
		t.Fatalf("LoadModelFile: %v", err)
	}
	t.Cleanup(model.Close)
	return model
}

func newTestContext(t *testing.T, config *ContextConfig) *Context {
	t.Helper()
	ctx, err := New(config)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	t.Cleanup(func() {
		if err := ctx.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
	})
	return ctx
}

// readFrame fills a new host frame from the next frame of r.
func readFrame(t *testing.T, r io.Reader) *Frame {
	t.Helper()
	frame, err := NewHostFrame(testDesc())
	if err != nil {
		t.Fatalf("NewHostFrame: %v", err)
	}
	planes, err := frame.Planes()
	if err != nil {
		t.Fatalf("Planes: %v", err)
	}
	for p := uint32(0); p < planes.NPlanes; p++ {
		buf, err := frame.PlaneBytes(p)
		if err != nil {
			t.Fatalf("PlaneBytes(%d): %v", p, err)
		}
		for row := uint32(0); row < planes.H[p]; row++ {
			off := uint64(row) * planes.Stride[p]
			if _, err := io.ReadFull(r, buf[off:off+uint64(planes.W[p])]); err != nil {
				t.Fatalf("read plane %d row %d: %v", p, row, err)
			}
		}
	}
	return frame
}

// scoredContext submits the first n frame pairs of the tracked fixture to a
// context with the test model registered; it does not flush.
func scoredContext(t *testing.T, n int) (*Context, *Model) {
	t.Helper()
	ref, err := os.Open(repoFile(t, "testdata", "ref_576x324_48f.yuv"))
	if err != nil {
		t.Fatalf("open ref: %v", err)
	}
	t.Cleanup(func() { _ = ref.Close() })
	dis, err := os.Open(repoFile(t, "testdata", "dis_576x324_48f.yuv"))
	if err != nil {
		t.Fatalf("open dis: %v", err)
	}
	t.Cleanup(func() { _ = dis.Close() })
	ctx := newTestContext(t, &ContextConfig{NSubsample: 1})
	model := loadModel(t)
	if err := ctx.UseModel(model); err != nil {
		t.Fatalf("UseModel: %v", err)
	}
	for i := range n {
		if err := ctx.Submit(readFrame(t, ref), readFrame(t, dis), uint64(i)); err != nil {
			t.Fatalf("Submit(%d): %v", i, err)
		}
	}
	return ctx, model
}

// requireStatus fails the test unless err is a *Error of the status and subject.
func requireStatus(t *testing.T, err error, want Status, subject string) {
	t.Helper()
	_ = statusError(t, err, want, subject)
}

// statusError is requireStatus returning the *Error it checked.
func statusError(t *testing.T, err error, want Status, subject string) *Error {
	t.Helper()
	var vErr *Error
	if !errors.As(err, &vErr) {
		t.Fatalf("error %v (%T) is not a *Error", err, err)
	}
	if vErr.Status != want {
		t.Fatalf("status = %v, want %v (%v)", vErr.Status, want, err)
	}
	if subject != "" && vErr.Subject != subject {
		t.Fatalf("subject = %q, want %q (%v)", vErr.Subject, subject, err)
	}
	return vErr
}

func TestModelLoadersAndGetters(t *testing.T) {
	model := loadModel(t)
	if got := model.Name(); got != "gen" {
		t.Errorf("Name() = %q, want gen", got)
	}
	count := model.FeatureCount()
	if count == 0 {
		t.Fatal("FeatureCount() = 0")
	}
	for i := range count {
		if model.FeatureName(i) == "" {
			t.Errorf("FeatureName(%d) is empty inside the count %d", i, count)
		}
	}
	if got := model.FeatureName(count); got != "" {
		t.Errorf("FeatureName(%d) = %q, want empty one past the count", count, got)
	}
	if got := model.Hash(); len(got) != 64 {
		t.Errorf("Hash() = %q, want 64 hex digits", got)
	}

	builtin, err := LoadModel(nil, ModelDefaultVersion())
	if err != nil {
		t.Fatalf("LoadModel(default version): %v", err)
	}
	if builtin.FeatureCount() == 0 {
		t.Error("the built-in model has no features")
	}
	builtin.Close()
	if _, err := LoadModel(nil, "no_such_version"); !errors.Is(err, ErrNotFound) {
		t.Errorf("LoadModel(unknown) = %v, want ErrNotFound", err)
	}

	spec, err := LoadModelSpec(nil, "version="+ModelDefaultVersion())
	if err != nil {
		t.Fatalf("LoadModelSpec: %v", err)
	}
	spec.Close()
	if _, err := LoadModelSpec(nil, "garbage"); !errors.Is(err, ErrInvalid) {
		t.Errorf("LoadModelSpec(garbage) = %v, want ErrInvalid", err)
	}

	if _, err := LoadModelFile(nil, filepath.Join(t.TempDir(), "absent.json")); !errors.Is(err, ErrIO) {
		t.Errorf("LoadModelFile(absent) = %v, want ErrIO", err)
	}
	empty := filepath.Join(t.TempDir(), "empty.json")
	if err := os.WriteFile(empty, []byte(`{"not": "a model"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadModelFile(nil, empty); !errors.Is(err, ErrInvalid) {
		t.Errorf("LoadModelFile(not a model) = %v, want ErrInvalid", err)
	}
}

func TestModelCloseContract(t *testing.T) {
	model, err := LoadModelFile(nil, testModelPath(t))
	if err != nil {
		t.Fatalf("LoadModelFile: %v", err)
	}
	model.Close()
	model.Close()
	var nilModel *Model
	nilModel.Close()
	if got := model.Name(); got != "" {
		t.Errorf("Name() after Close = %q, want empty", got)
	}
	if got := model.FeatureCount(); got != 0 {
		t.Errorf("FeatureCount() after Close = %d, want 0", got)
	}
}

func TestFrameLifecycleAndPlanes(t *testing.T) {
	frame, err := NewHostFrame(FrameDesc{PixFmt: 1, Bpc: 8, W: 16, H: 8})
	if err != nil {
		t.Fatalf("NewHostFrame: %v", err)
	}
	planes, err := frame.Planes()
	if err != nil {
		t.Fatalf("Planes: %v", err)
	}
	if planes.NPlanes != 3 || planes.W != [3]uint32{16, 8, 8} || planes.H != [3]uint32{8, 4, 4} || planes.Bpc != 8 {
		t.Fatalf("Planes = %+v, want a 16x8 4:2:0 8-bit frame", planes)
	}
	for p := uint32(0); p < planes.NPlanes; p++ {
		buf, err := frame.PlaneBytes(p)
		if err != nil {
			t.Fatalf("PlaneBytes(%d): %v", p, err)
		}
		if uint64(len(buf)) != planes.Stride[p]*uint64(planes.H[p]) {
			t.Errorf("len(PlaneBytes(%d)) = %d, want stride*h = %d", p, len(buf), planes.Stride[p]*uint64(planes.H[p]))
		}
		buf[0] = byte(p + 1)
		again, err := frame.PlaneBytes(p)
		if err != nil || again[0] != byte(p+1) {
			t.Errorf("plane %d does not alias frame memory: %v %v", p, again[:1], err)
		}
	}
	// Boundary: one past the last plane.
	_, err = frame.PlaneBytes(planes.NPlanes)
	requireStatus(t, err, StatusEInvalid, "plane")

	frame.Close()
	frame.Close()
	var nilFrame *Frame
	nilFrame.Close()
	if _, err := frame.Planes(); !errors.Is(err, ErrInvalid) {
		t.Errorf("Planes after Close = %v, want ErrInvalid", err)
	}
	if _, err := frame.PlaneBytes(0); !errors.Is(err, ErrInvalid) {
		t.Errorf("PlaneBytes after Close = %v, want ErrInvalid", err)
	}
}

func TestNewHostFrameRejectsBadDescriptors(t *testing.T) {
	cases := map[string]FrameDesc{
		"zero_width":      {PixFmt: 1, Bpc: 8, W: 0, H: 8},
		"zero_height":     {PixFmt: 1, Bpc: 8, W: 8, H: 0},
		"unknown_format":  {PixFmt: 9, Bpc: 8, W: 8, H: 8},
		"unsupported_bpc": {PixFmt: 1, Bpc: 7, W: 8, H: 8},
	}
	for name, desc := range cases {
		t.Run(name, func(t *testing.T) {
			if frame, err := NewHostFrame(desc); !errors.Is(err, ErrInvalid) {
				frame.Close()
				t.Fatalf("NewHostFrame(%+v) = %v, want ErrInvalid", desc, err)
			}
		})
	}
	smallest, err := NewHostFrame(FrameDesc{PixFmt: 1, Bpc: 8, W: 1, H: 1})
	if err != nil {
		t.Fatalf("NewHostFrame(1x1) = %v, want success at the boundary", err)
	}
	smallest.Close()
}

func TestSubmitFlushAndScores(t *testing.T) {
	const frames = 4
	ctx, model := scoredContext(t, frames)

	if err := ctx.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	requireStatus(t, ctx.Flush(), StatusEInvalid, "context")

	first, err := ctx.ScoreFrame(model, 0)
	if err != nil || first.Value <= 0 || first.Value > 100 {
		t.Fatalf("ScoreFrame(0) = (%+v, %v), want a score in (0, 100]", first, err)
	}
	pooled, err := ctx.ScorePooled(model, PoolMean, 0, frames-1)
	if err != nil {
		t.Fatalf("ScorePooled: %v", err)
	}
	if pooled.Pool != PoolMean || pooled.First != 0 || pooled.Last != frames-1 || pooled.Value <= 0 {
		t.Fatalf("ScorePooled = %+v", pooled)
	}
	// Boundary: a one-frame range pools to that frame's score, to the bit.
	one, err := ctx.ScorePooled(model, PoolMean, 0, 0)
	if err != nil || math.Float64bits(one.Value) != math.Float64bits(first.Value) {
		t.Fatalf("ScorePooled(0,0) = (%v, %v), want ScoreFrame(0) = %v", one.Value, err, first.Value)
	}

	// Negative cases.
	_, err = ctx.ScoreFrame(model, 99)
	requireStatus(t, err, StatusEInvalid, "")
	_, err = ctx.ScoreFrame(nil, 0)
	requireStatus(t, err, StatusEInvalid, "model")
	_, err = ctx.ScorePooled(nil, PoolMean, 0, 0)
	requireStatus(t, err, StatusEInvalid, "model")
	_, err = ctx.ScorePooled(model, 77, 0, frames-1)
	requireStatus(t, err, StatusEInvalid, "pool")
	_, err = ctx.ScorePooled(model, PoolMean, frames-1, 0)
	requireStatus(t, err, StatusEInvalid, "first")
	if _, err := ctx.ScorePooled(model, PoolMean, 0, 99); !errors.Is(err, ErrPending) {
		t.Errorf("ScorePooled past the last frame = %v, want ErrPending", err)
	}
	spare, err := NewHostFrame(testDesc())
	if err != nil {
		t.Fatal(err)
	}
	spare2, err := NewHostFrame(testDesc())
	if err != nil {
		t.Fatal(err)
	}
	requireStatus(t, ctx.Submit(spare, spare2, frames), StatusEInvalid, "context")
	spare.Close()
}

func TestSubmitRejectsNilFramesAndConsumesBoth(t *testing.T) {
	ctx := newTestContext(t, nil)
	requireStatus(t, ctx.Submit(nil, nil, 0), StatusEInvalid, "reference")
	frame, err := NewHostFrame(testDesc())
	if err != nil {
		t.Fatal(err)
	}
	requireStatus(t, ctx.Submit(frame, nil, 0), StatusEInvalid, "distorted")
	// Submit consumed the reference even though it failed: the handle is gone.
	if _, err := frame.Planes(); !errors.Is(err, ErrInvalid) {
		t.Errorf("a submitted frame stays usable: %v", err)
	}
}

func TestAdmit(t *testing.T) {
	ctx := newTestContext(t, nil)
	frame, err := NewHostFrame(testDesc())
	if err != nil {
		t.Fatal(err)
	}
	defer frame.Close()
	if err := ctx.Admit(frame); err != nil {
		t.Errorf("Admit(host frame) = %v, want nil", err)
	}
	requireStatus(t, ctx.Admit(nil), StatusEInvalid, "frame")
}

func TestUseModelAndSetOption(t *testing.T) {
	ctx := newTestContext(t, nil)
	requireStatus(t, ctx.UseModel(nil), StatusEInvalid, "model")
	if err := ctx.UseModel(loadModel(t)); err != nil {
		t.Fatalf("UseModel: %v", err)
	}
	if err := ctx.SetOption("check_sample_range", "1"); err != nil {
		t.Errorf("SetOption(check_sample_range, 1) = %v", err)
	}
	requireStatus(t, ctx.SetOption("check_sample_range", "zz"), StatusEInvalid, "check_sample_range")
	if err := ctx.SetOption("no_such_option", "1"); !errors.Is(err, ErrNotFound) {
		t.Errorf("SetOption(unknown) = %v, want ErrNotFound", err)
	}
}

func TestProvenanceAccessors(t *testing.T) {
	ctx, model := scoredContext(t, 2)
	if err := ctx.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	mp, err := ctx.ModelProvenance(0)
	if err != nil || mp.Name != "gen" || mp.Sha256 != model.Hash() {
		t.Fatalf("ModelProvenance(0) = (%+v, %v), want the loaded model", mp, err)
	}
	if _, err := ctx.ModelProvenance(99); !errors.Is(err, ErrNotFound) {
		t.Errorf("ModelProvenance(99) = %v, want ErrNotFound", err)
	}
	fp, err := ctx.FeatureProvenanceAt(0)
	if err != nil || fp.Feature == "" || fp.Extractor == "" {
		t.Fatalf("FeatureProvenanceAt(0) = (%+v, %v)", fp, err)
	}
	if _, err := ctx.FeatureProvenanceAt(9999); !errors.Is(err, ErrNotFound) {
		t.Errorf("FeatureProvenanceAt(9999) = %v, want ErrNotFound", err)
	}
	byName, err := ctx.FeatureProvenance(fp.Feature)
	if err != nil || byName.Feature != fp.Feature {
		t.Fatalf("FeatureProvenance(%q) = (%+v, %v)", fp.Feature, byName, err)
	}
	if _, err := ctx.FeatureProvenance("no_such_feature"); !errors.Is(err, ErrNotFound) {
		t.Errorf("FeatureProvenance(unknown) = %v, want ErrNotFound", err)
	}
	score, err := ctx.FeatureScore(fp.Feature, 0)
	if err != nil || score.Index != 0 {
		t.Fatalf("FeatureScore(%q, 0) = (%+v, %v)", fp.Feature, score, err)
	}
}

func TestProvenanceJSONAndAnnotations(t *testing.T) {
	ctx, _ := scoredContext(t, 2)
	if err := ctx.Annotate("run", "go-binding-test"); err != nil {
		t.Fatalf("Annotate: %v", err)
	}
	annotation, err := ctx.Annotation(0)
	if err != nil || annotation.Key != "run" || annotation.Value != "go-binding-test" {
		t.Fatalf("Annotation(0) = (%+v, %v)", annotation, err)
	}
	vErr := statusError(t, mustErr(t, ctx.Annotation, 5), StatusENotFound, "index")
	if vErr.Function != "vmafx_context_annotation" {
		t.Errorf("Function = %q", vErr.Function)
	}
	digest := "sha256:0000000000000000000000000000000000000000000000000000000000000001"
	if err := ctx.SetEncodeRecord(digest); err != nil {
		t.Fatalf("SetEncodeRecord: %v", err)
	}
	requireStatus(t, ctx.SetEncodeRecord("not a digest"), StatusEInvalid, "record")

	text, err := ctx.ProvenanceJson(0)
	if err != nil {
		t.Fatalf("ProvenanceJson: %v", err)
	}
	var record map[string]any
	if err := json.Unmarshal([]byte(text), &record); err != nil || len(record) == 0 {
		t.Fatalf("ProvenanceJson is not a JSON object: %v\n%s", err, text)
	}
}

// mustErr calls an index getter that is expected to fail and returns its error.
func mustErr[T any](t *testing.T, fn func(uint32) (T, error), index uint32) error {
	t.Helper()
	_, err := fn(index)
	if err == nil {
		t.Fatalf("index %d unexpectedly succeeded", index)
	}
	return err
}

func TestUseFeatureSpec(t *testing.T) {
	ctx := newTestContext(t, nil)
	if err := ctx.UseFeatureSpec("psnr"); err != nil {
		t.Errorf("UseFeatureSpec(psnr) = %v", err)
	}
	if err := ctx.UseFeatureSpec("no_such_extractor"); !errors.Is(err, ErrNotFound) {
		t.Errorf("UseFeatureSpec(unknown) = %v, want ErrNotFound", err)
	}
}

func TestReportWrite(t *testing.T) {
	ctx, _ := scoredContext(t, 2)
	if err := ctx.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	report := filepath.Join(t.TempDir(), "report.json")
	if err := ctx.ReportWrite(report, ReportFormatJson, 0, "%.6f"); err != nil {
		t.Fatalf("ReportWrite: %v", err)
	}
	raw, err := os.ReadFile(report)
	if err != nil || !json.Valid(raw) {
		t.Fatalf("the report is not valid JSON (%v)", err)
	}
	if err := ctx.ReportWrite("", ReportFormatJson, 0, "%.6f"); err == nil {
		t.Error("ReportWrite(\"\") must fail")
	}
}

func TestErrorMessageAndUnwrap(t *testing.T) {
	cases := []struct {
		err     *Error
		message string
		is      error
	}{
		{&Error{Function: "f", Status: StatusEInvalid, Message: "bad", Subject: "x"}, "f: VMAFX_E_INVALID: bad [x]", ErrInvalid},
		{&Error{Function: "f", Status: StatusENotFound, Message: "gone"}, "f: VMAFX_E_NOTFOUND: gone", ErrNotFound},
		{&Error{Function: "f", Status: StatusENoMem}, "f: VMAFX_E_NOMEM", ErrNoMem},
		{&Error{Function: "f", Status: StatusPending}, "f: VMAFX_PENDING", ErrPending},
	}
	for _, tc := range cases {
		if got := tc.err.Error(); got != tc.message {
			t.Errorf("Error() = %q, want %q", got, tc.message)
		}
		if got := tc.err.Unwrap(); !errors.Is(got, tc.is) {
			t.Errorf("Unwrap() = %v, want %v", got, tc.is)
		}
	}
	if errors.Is((&Error{Status: StatusEInvalid}).Unwrap(), ErrNotFound) {
		t.Error("StatusEInvalid must not unwrap to ErrNotFound")
	}
}

func TestCodecAndTinyResizeWithoutATinyModel(t *testing.T) {
	ctx := newTestContext(t, nil)
	// No codec-aware tiny model is registered, so the codec context has nothing
	// to attach to.
	requireStatus(t, ctx.SetCodecContext("h264", "medium", 23), StatusEInvalid, "context")
	if err := ctx.SetTinyResize(1); err != nil {
		t.Errorf("SetTinyResize(1) = %v", err)
	}
	requireStatus(t, ctx.SetTinyResize(99), StatusEInvalid, "mode")
}
