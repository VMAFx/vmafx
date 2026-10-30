// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/vmafx_errors.go — maps the structured errors of pkg/vmafx onto
// the errno contract of this package (errors.go, ADR-0931).
//
// The in-process paths run on the generated pkg/vmafx binding, whose calls
// fail with *vmafx.Error (a stable Status, the engine's negative errno and a
// message naming the parameter). Callers of this package still branch on
// ErrInvalidArgument, ErrModelNotFound, ErrOutOfMemory and ErrPictureRead and
// on os.ErrInvalid / os.ErrNotExist, so every failure is translated to the
// negative errno the engine reported and then to the same sentinel as before.
// The *vmafx.Error stays in the chain, so errors.As and errors.Is on the
// vmafx sentinels keep working next to the libvmaf ones.

//go:build cgo

package libvmaf

import (
	"errors"
	"fmt"
	"syscall"

	"github.com/VMAFx/vmafx/pkg/vmafx"
)

// errnoOf returns the negative errno a pkg/vmafx error stands for. The status
// is the stable classification: the four statuses the historic errno contract
// knows (EINVAL, ENOMEM, ENOENT, EIO) map to their errno whatever the errno
// field holds, because a failure raised by the facade itself carries a
// placeholder there. Any other status falls back to the engine errno the error
// carries; ok is false when there is none, for anything that is not a
// *vmafx.Error, and for StatusEDevice, whose errno field is a backend code and
// must not be read as an errno.
func errnoOf(err error) (rc int, ok bool) {
	var vErr *vmafx.Error
	if !errors.As(err, &vErr) {
		return 0, false
	}
	switch vErr.Status {
	case vmafx.StatusEInvalid:
		return -int(syscall.EINVAL), true
	case vmafx.StatusENoMem:
		return -int(syscall.ENOMEM), true
	case vmafx.StatusENotFound:
		return -int(syscall.ENOENT), true
	case vmafx.StatusEIO:
		return -int(syscall.EIO), true
	case vmafx.StatusEDevice:
		return 0, false
	default:
		if vErr.Errno == 0 {
			return 0, false
		}
		return -absInt(int(vErr.Errno)), true
	}
}

func absInt(n int) int {
	if n < 0 {
		return -n
	}
	return n
}

// mapVmafxError converts a pkg/vmafx error into this package's error contract.
// call names the vmafx entry point. A nil error stays nil. An error with no
// errno meaning is wrapped with the call name only.
func mapVmafxError(call string, err error) error {
	if err == nil {
		return nil
	}
	rc, ok := errnoOf(err)
	if !ok {
		return fmt.Errorf("libvmaf: %s: %w", call, err)
	}
	return fmt.Errorf("%w [%w]", mapErrno(call, rc), err)
}

// mapModelLoadError converts a failed model load. The contract of errors.go
// comes from vmaf_model_load_from_path, which reports a path that exists but
// cannot be read or parsed as a model (a directory, a .pkl file) as EINVAL, so
// the file statuses of vmafx map to ErrInvalidArgument here, not to their own
// errno. Every other status maps as usual.
func mapModelLoadError(err error) error {
	var vErr *vmafx.Error
	if errors.As(err, &vErr) {
		switch vErr.Status {
		case vmafx.StatusENotFound, vmafx.StatusEIO, vmafx.StatusENotSup:
			return fmt.Errorf("%w [%w]", mapErrno("vmafx_model_load_file", -int(syscall.EINVAL)), err)
		}
	}
	return mapVmafxError("vmafx_model_load_file", err)
}

// mapCloseError converts the error of a context close the way mapCloseRC maps
// the status of vmaf_close: only an exact success is success, and a failure
// keeps the *vmafx.Error in its chain.
func mapCloseError(err error) error {
	if err == nil {
		return nil
	}
	rc, ok := errnoOf(err)
	if !ok {
		return fmt.Errorf("libvmaf: vmafx_context_destroy: %w", err)
	}
	return fmt.Errorf("%w [%w]", mapCloseRC(closeStatus(err, rc)), err)
}

// closeStatus returns the status mapCloseRC judges. errnoOf normalises the
// sign for mapErrno, which would hide a positive status of the engine: outside
// the four statuses that fix their errno, a positive errno field stays positive
// so that mapCloseRC reports it as unexpected instead of reading it as an errno.
func closeStatus(err error, rc int) int {
	var vErr *vmafx.Error
	if !errors.As(err, &vErr) || vErr.Errno <= 0 {
		return rc
	}
	switch vErr.Status {
	case vmafx.StatusEInvalid, vmafx.StatusENoMem, vmafx.StatusENotFound, vmafx.StatusEIO:
		return rc
	default:
		return int(vErr.Errno)
	}
}
