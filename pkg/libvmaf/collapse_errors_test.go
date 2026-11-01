// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/collapse_errors_test.go — the error contract of the in-process
// paths (#2434): typed errors for corrupt inputs, the errno mapping, the model
// loader and the argument checks.

//go:build cgo

package libvmaf

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/vmafx"
)

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
	positive := mapCloseError(&vmafx.Error{Status: vmafx.StatusEInternal, Errno: 12})
	if positive == nil || errors.Is(positive, ErrOutOfMemory) || !strings.Contains(positive.Error(), "unexpected positive status 12") {
		t.Fatalf("mapCloseError(positive status) = %v, want the positive status reported", positive)
	}
	if got := mapCloseError(&vmafx.Error{Status: vmafx.StatusENoMem, Errno: 12}); !errors.Is(got, ErrOutOfMemory) {
		t.Fatalf("mapCloseError(ENOMEM with a positive errno field) = %v, want ErrOutOfMemory", got)
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
	var vErr *vmafx.Error
	_, err = loadScoringModel(t.TempDir(), "m")
	if errors.Is(err, ErrModelNotFound) || !errors.Is(err, ErrInvalidArgument) ||
		!errors.Is(err, os.ErrInvalid) || !errors.As(err, &vErr) {
		t.Errorf("directory as model: %v, want ErrInvalidArgument, os.ErrInvalid and the vmafx error", err)
	}
	if err := ValidateModel(t.TempDir()); !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("ValidateModel(directory) = %v, want ErrInvalidArgument", err)
	}
	pkl := filepath.Join(t.TempDir(), "legacy.pkl")
	if err := os.WriteFile(pkl, []byte("not read"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err = loadScoringModel(pkl, "m")
	if !errors.Is(err, ErrInvalidArgument) || !errors.Is(err, os.ErrInvalid) || !errors.As(err, &vErr) {
		t.Errorf("pkl model: %v, want ErrInvalidArgument, os.ErrInvalid and the vmafx error", err)
	}
	if err := ValidateModel(pkl); !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("ValidateModel(pkl) = %v, want ErrInvalidArgument", err)
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
		{"unknown_pixel_format", 8, 8, 8, PixelFormat(4), false},
		{"negative_pixel_format", 8, 8, 8, PixelFormat(-1), false},
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
