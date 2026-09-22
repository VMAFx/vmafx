// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/corpus/cleanup.go — shared handling for the package's best-effort
// teardown steps.
//
// The corpus drivers return encode results, not errors, so a failed close or
// a failed unlink has nowhere to propagate to. Dropping it silently is what
// leaves an operator hunting for stray passlogfile sidecars in the scratch
// directory, so the failures are reported here instead of discarded.

package corpus

import (
	"errors"
	"log/slog"
	"os"
)

// warnClose reports a Close failure on a handle the caller can no longer act
// on. Read handles are the normal case: the data has already been consumed,
// so the only useful response is to make the failure visible.
func warnClose(what string, err error) {
	if err != nil {
		slog.Warn("corpus: close "+what, "error", err)
	}
}

// removeScratchFile unlinks a driver-generated temporary file. "Already gone"
// is the expected outcome for encoder sidecars the encoder did not create, so
// only a real failure is reported.
func removeScratchFile(path string) {
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		slog.Warn("corpus: remove scratch file", "path", path, "error", err)
	}
}

// removeScratchTree removes a scratch directory the driver created itself.
func removeScratchTree(dir string) {
	if err := os.RemoveAll(dir); err != nil {
		slog.Warn("corpus: remove scratch dir", "path", dir, "error", err)
	}
}
