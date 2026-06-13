// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/errors_test.go — round-trip tests for the libvmaf negative-errno
// to typed-Go-error mapping (ADR-0931 Phase 1).

package libvmaf

import (
	"errors"
	"os"
	"syscall"
	"testing"
)

func TestMapErrno_SuccessIsNil(t *testing.T) {
	if err := mapErrno("vmaf_init", 0); err != nil {
		t.Errorf("mapErrno(call, 0) = %v, want nil", err)
	}
	if err := mapErrno("vmaf_init", 1); err != nil {
		t.Errorf("mapErrno(call, 1) = %v, want nil (positive rc treated as success)", err)
	}
}

func TestMapErrno_TypedSentinels(t *testing.T) {
	cases := []struct {
		name string
		rc   int
		want error
	}{
		{"EINVAL", -int(syscall.EINVAL), ErrInvalidArgument},
		{"ENOMEM", -int(syscall.ENOMEM), ErrOutOfMemory},
		{"ENOENT", -int(syscall.ENOENT), ErrModelNotFound},
		{"EIO", -int(syscall.EIO), ErrPictureRead},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := mapErrno("vmaf_test", tc.rc)
			if err == nil {
				t.Fatal("expected error, got nil")
			}
			if !errors.Is(err, tc.want) {
				t.Errorf("errors.Is(_, %v) = false; err = %v", tc.want, err)
			}
		})
	}
}

func TestMapErrno_WrapsStdlibSentinels(t *testing.T) {
	// ErrInvalidArgument wraps os.ErrInvalid; ErrModelNotFound wraps
	// os.ErrNotExist.  Verify the chain propagates through mapErrno.
	einval := mapErrno("vmaf_init", -int(syscall.EINVAL))
	if !errors.Is(einval, os.ErrInvalid) {
		t.Errorf("expected EINVAL to wrap os.ErrInvalid; got %v", einval)
	}
	enoent := mapErrno("vmaf_model_load_from_path", -int(syscall.ENOENT))
	if !errors.Is(enoent, os.ErrNotExist) {
		t.Errorf("expected ENOENT to wrap os.ErrNotExist; got %v", enoent)
	}
}

func TestMapErrno_UnknownErrnoFallback(t *testing.T) {
	// Use a high errno value that does not have a sentinel in mapErrno.
	// EOWNERDEAD (130) is not in the typed mapping; expect a fallthrough
	// "returned %d" error that is NOT any of the sentinels.
	err := mapErrno("vmaf_test", -130)
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	for _, sentinel := range []error{
		ErrInvalidArgument, ErrOutOfMemory, ErrModelNotFound, ErrPictureRead,
	} {
		if errors.Is(err, sentinel) {
			t.Errorf("fallback errno mistakenly matched sentinel %v: %v", sentinel, err)
		}
	}
}
