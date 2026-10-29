// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package vmafx

import (
	"errors"
	"os"
	"syscall"
	"testing"
)

func TestStatus(t *testing.T) {
	cases := []struct {
		status Status
		raw    int32
		name   string
		errno  syscall.Errno
		err    error
	}{
		{StatusOK, 0, "VMAFX_OK", 0, nil},
		{StatusPending, 1, "VMAFX_PENDING", syscall.EAGAIN, nil},
		{StatusEInvalid, -1, "VMAFX_E_INVALID", syscall.EINVAL, ErrInvalid},
		{StatusENoMem, -2, "VMAFX_E_NOMEM", syscall.ENOMEM, ErrNoMem},
		{StatusENotSup, -3, "VMAFX_E_NOTSUP", syscall.ENOTSUP, ErrNotSup},
		{StatusENotFound, -4, "VMAFX_E_NOTFOUND", syscall.ENOENT, ErrNotFound},
		{StatusEBusy, -5, "VMAFX_E_BUSY", syscall.EBUSY, ErrBusy},
		{StatusEDevice, -6, "VMAFX_E_DEVICE", syscall.EIO, ErrDevice},
		{StatusEIO, -7, "VMAFX_E_IO", syscall.EIO, ErrIO},
		{StatusERange, -8, "VMAFX_E_RANGE", syscall.ERANGE, ErrRange},
		{StatusEInternal, -9, "VMAFX_E_INTERNAL", syscall.EIO, ErrInternal},
		{StatusETimeout, -10, "VMAFX_E_TIMEOUT", syscall.ETIMEDOUT, ErrTimeout},
		{StatusEABI, -11, "VMAFX_E_ABI", syscall.EINVAL, ErrABI},
		{StatusEMismatch, -12, "VMAFX_E_MISMATCH", syscall.EBADMSG, ErrMismatch},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if int32(tc.status) != tc.raw || Status(tc.raw) != tc.status {
				t.Errorf("status %v did not round-trip through %d", tc.status, tc.raw)
			}
			if got := tc.status.String(); got != tc.name {
				t.Errorf("String() = %q, want %q", got, tc.name)
			}
			if got := tc.status.Errno(); got != tc.errno {
				t.Errorf("Errno() = %v, want %v", got, tc.errno)
			}
			if got := tc.status.Err(); got != tc.err {
				t.Errorf("Err() = %v, want sentinel %v", got, tc.err)
			}
		})
	}
}

func TestStatusUnknown(t *testing.T) {
	for _, s := range []Status{-2147483648, -13, 2, 2147483647} {
		if s.Errno() != 0 {
			t.Errorf("unknown status %d has errno %v", s, s.Errno())
		}
		if s.String() == "" {
			t.Errorf("unknown status %d has no description", s)
		}
		if (s.Err() != nil) != (s < 0) {
			t.Errorf("status %d has incorrect error polarity: %v", s, s.Err())
		}
	}
	if got := Status(-13).String(); got != "Status(-13)" {
		t.Errorf("unknown String() = %q", got)
	}
}

func TestStatusErrorsIs(t *testing.T) {
	for _, tc := range []struct {
		status Status
		target error
	}{
		{StatusEInvalid, os.ErrInvalid},
		{StatusENotFound, os.ErrNotExist},
		{StatusENotSup, errors.ErrUnsupported},
	} {
		if !errors.Is(tc.status.Err(), tc.target) {
			t.Errorf("%s does not wrap %v", tc.status, tc.target)
		}
		if errors.Is(StatusEDevice.Err(), tc.target) {
			t.Errorf("device error unexpectedly wraps %v", tc.target)
		}
	}
}
