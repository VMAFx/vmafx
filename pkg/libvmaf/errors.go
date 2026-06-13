// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/errors.go — typed errors mapped from libvmaf negative-errno
// return codes.
//
// libvmaf's public C ABI returns 0 on success and `< 0` (a negative errno)
// on error.  Go callers benefit from typed sentinels for programmatic
// recovery — e.g. `errors.Is(err, libvmaf.ErrModelNotFound)` — rather than
// string-matching on the wrapped `fmt.Errorf` value.
//
// The mapping is contract-frozen by ADR-0931 (MCP cgo direct, Phase 1).
// When extending the mapping, update the ADR's "Error mapping" table and
// the corresponding entries in pkg/libvmaf/errors_test.go.

package libvmaf

import (
	"errors"
	"fmt"
	"os"
	"syscall"
)

// Sentinel errors returned by the direct-cgo scoring path.
//
// Callers may branch via `errors.Is`:
//
//	if errors.Is(err, libvmaf.ErrModelNotFound) { ... }
var (
	// ErrInvalidArgument is returned when libvmaf rejects a parameter
	// (typically `-EINVAL` from vmaf_init / vmaf_read_pictures /
	// vmaf_model_load_from_path).  Wraps os.ErrInvalid so generic
	// "is this a bad-input error?" checks work.
	ErrInvalidArgument = fmt.Errorf("libvmaf: invalid argument: %w", os.ErrInvalid)

	// ErrOutOfMemory is returned when libvmaf or the cgo bridge cannot
	// allocate (`-ENOMEM` or a cgo malloc failure).
	ErrOutOfMemory = errors.New("libvmaf: out of memory")

	// ErrModelNotFound is returned when vmaf_model_load_from_path fails
	// to open the model file (`-ENOENT`).  Wraps os.ErrNotExist so the
	// caller's existing path-not-found checks fire transparently.
	ErrModelNotFound = fmt.Errorf("libvmaf: model not found: %w", os.ErrNotExist)

	// ErrPictureRead is returned when reading raw YUV planes from disk
	// fails (`-EIO` from the I/O loop) or when vmaf_read_pictures rejects
	// a malformed picture.
	ErrPictureRead = errors.New("libvmaf: picture read failed")
)

// mapErrno converts a libvmaf negative-errno return code into a Go error.
// `call` names the libvmaf entry point (e.g. "vmaf_init") and is used in
// the fallback "<call> returned %d" string when no typed sentinel matches.
//
// Passing `rc >= 0` returns nil — by libvmaf convention, success is `0` and
// any positive return value is treated as success (no libvmaf function in
// the public ABI returns positive values today, but the contract guards
// against accidental future regressions).
func mapErrno(call string, rc int) error {
	if rc >= 0 {
		return nil
	}
	// libvmaf returns negative errno; flip to positive for syscall.Errno.
	errno := syscall.Errno(-rc)
	switch errno {
	case syscall.EINVAL:
		return fmt.Errorf("libvmaf: %s: %w", call, ErrInvalidArgument)
	case syscall.ENOMEM:
		return fmt.Errorf("libvmaf: %s: %w", call, ErrOutOfMemory)
	case syscall.ENOENT:
		return fmt.Errorf("libvmaf: %s: %w", call, ErrModelNotFound)
	case syscall.EIO:
		return fmt.Errorf("libvmaf: %s: %w", call, ErrPictureRead)
	default:
		return fmt.Errorf("libvmaf: %s returned %d (%s)", call, rc, errno.Error())
	}
}
