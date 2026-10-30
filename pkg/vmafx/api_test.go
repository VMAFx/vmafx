// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package vmafx

import (
	"errors"
	"os"
	"syscall"
	"testing"
)

func TestLibraryFunctions(t *testing.T) {
	if got := VersionString(); got == "" {
		t.Fatal("VersionString() returned empty string")
	}
	major, minor, patch := ABIVersion()
	if major != ABIVersionMajor || minor != ABIVersionMinor || patch != ABIVersionPatch {
		t.Fatalf(
			"ABIVersion() = (%d, %d, %d), want (%d, %d, %d)",
			major, minor, patch, ABIVersionMajor, ABIVersionMinor, ABIVersionPatch,
		)
	}
	if got := StatusName(StatusEInvalid); got != "VMAFX_E_INVALID" {
		t.Fatalf("StatusName(StatusEInvalid) = %q, want VMAFX_E_INVALID", got)
	}
	if got := ModelDefaultVersion(); got == "" {
		t.Fatal("ModelDefaultVersion() returned empty string")
	}
	firstModel := ModelBuiltinNext("")
	if firstModel == "" {
		t.Fatal("ModelBuiltinNext(\"\") returned empty string")
	}
	if got := BackendName(BackendCpu); got != "cpu" {
		t.Fatalf("BackendName(BackendCpu) = %q, want cpu", got)
	}
}

func TestContextImportAndPooledScore(t *testing.T) {
	ctx, err := New(&ContextConfig{NThreads: 1})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer func() {
		if closeErr := ctx.Close(); closeErr != nil {
			t.Errorf("Close: %v", closeErr)
		}
	}()
	if ctx.LibvmafHandle() == nil {
		t.Fatal("LibvmafHandle() == nil")
	}
	prov, err := ctx.Provenance()
	if err != nil {
		t.Fatalf("Provenance: %v", err)
	}
	if prov.ABIMajor != ABIVersionMajor || prov.NThreads != 1 {
		t.Fatalf("Provenance = %+v, want ABIMajor=%d NThreads=1", prov, ABIVersionMajor)
	}
	if err := ctx.ImportScore("x", 0, 1.0); err != nil {
		t.Fatalf("ImportScore(0): %v", err)
	}
	if err := ctx.ImportScore("x", 1, 2.0); err != nil {
		t.Fatalf("ImportScore(1): %v", err)
	}
	score, err := ctx.FeatureScore("x", 0)
	if err != nil {
		t.Fatalf("FeatureScore(0): %v", err)
	}
	if score.Value != 1.0 {
		t.Fatalf("FeatureScore(0).Value = %v, want 1.0", score.Value)
	}
	pooled, err := ctx.FeatureScorePooled("x", PoolMean, 0, 1)
	if err != nil {
		t.Fatalf("FeatureScorePooled: %v", err)
	}
	if pooled.Value != 1.5 || pooled.First != 0 || pooled.Last != 1 || pooled.Pool != PoolMean {
		t.Fatalf("FeatureScorePooled = %+v, want Value=1.5 First=0 Last=1", pooled)
	}
}

func TestContextNegativeAndCloseContract(t *testing.T) {
	if _, err := New(&ContextConfig{ImportRetryWaitNs: 600_000_000_001}); !errors.Is(err, ErrRange) {
		t.Fatalf("New(out-of-range wait) = %v, want ErrRange", err)
	}
	ctx, err := NewContext(nil)
	if err != nil {
		t.Fatalf("NewContext(nil): %v", err)
	}
	_, err = ctx.ExtractorInfo(99999)
	if !errors.Is(err, ErrNotFound) || !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("ExtractorInfo(99999) = %v, want ErrNotFound / os.ErrNotExist", err)
	}
	var vErr *Error
	if !errors.As(err, &vErr) {
		t.Fatalf("ExtractorInfo error type = %T, want *Error", err)
	}
	if vErr.Status != StatusENotFound || vErr.Subject != "index" || vErr.Errno != -int32(syscall.ENOENT) {
		t.Fatalf("Error = %+v, want StatusENotFound Subject=index Errno=-ENOENT", vErr)
	}
	if err := ctx.Close(); err != nil {
		t.Fatalf("first Close: %v", err)
	}
	if err := ctx.Close(); err != nil {
		t.Fatalf("second Close: %v", err)
	}
	var nilCtx *Context
	if err := nilCtx.Close(); err != nil {
		t.Fatalf("nil Close: %v", err)
	}
	if _, err := ctx.Provenance(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("Provenance after Close = %v, want ErrInvalid", err)
	}
}

func TestContextPendingBoundary(t *testing.T) {
	ctx, err := New(nil)
	if err != nil {
		t.Fatalf("New(nil): %v", err)
	}
	defer func() {
		if closeErr := ctx.Close(); closeErr != nil {
			t.Errorf("Close: %v", closeErr)
		}
	}()
	if err := ctx.ImportScore("x", 0, 1.0); err != nil {
		t.Fatalf("ImportScore(0): %v", err)
	}
	_, err = ctx.FeatureScorePooled("x", PoolMean, 0, 1)
	if !errors.Is(err, ErrPending) {
		t.Fatalf("FeatureScorePooled(unscored frame 1) = %v, want ErrPending", err)
	}
	var vErr *Error
	if !errors.As(err, &vErr) || vErr.Status != StatusPending {
		t.Fatalf("pending error = %+v, want *Error with StatusPending", err)
	}
	if err := ctx.ImportScore("x", 1, 2.0); err != nil {
		t.Fatalf("ImportScore(1): %v", err)
	}
	pooled, err := ctx.FeatureScorePooled("x", PoolMean, 0, 1)
	if err != nil || pooled.Value != 1.5 {
		t.Fatalf("FeatureScorePooled after frame 1 = (%+v, %v), want Value=1.5", pooled, err)
	}
}
