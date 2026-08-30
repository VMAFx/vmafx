// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/ids.go — run-id, timestamp and source-hash helpers.
//
// The corpus row's provenance triple (run_id / timestamp / src_sha256) has to
// render exactly as the Python writer does: uuid4().hex is 32 lowercase hex
// chars, aiutils.time_utils.now_iso_8601 is second-precision UTC with a
// numeric "+00:00" offset (never "Z"), and aiutils.file_utils.sha256 streams
// the file in 1 MiB chunks.

package corpus

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"time"
)

// NewRunID returns a 32-character lowercase hex token, matching uuid4().hex.
//
// The Python writer stamps a fresh uuid4 per row purely as a correlation
// handle, so a 128-bit random token from the same entropy source is an exact
// behavioural match; the RFC-4122 version / variant bits carry no meaning to
// any consumer of the column.
func NewRunID() string {
	buf := make([]byte, 16)
	if _, err := rand.Read(buf); err != nil {
		// crypto/rand failure is fatal on every supported platform; fall
		// back to a time-derived token rather than aborting the sweep.
		return fmt.Sprintf("%032x", time.Now().UnixNano())
	}
	return hex.EncodeToString(buf)
}

// shortToken returns an 8-character hex token, matching uuid4().hex[:8].
func shortToken() string {
	return NewRunID()[:8]
}

// UTCNowISO8601 returns the current UTC time as a second-precision ISO-8601
// string with an explicit numeric offset, e.g. "2026-05-16T12:34:56+00:00".
//
// The "-07:00" layout element (rather than "Z07:00") is what forces the
// numeric form Python's datetime.isoformat() produces for a tz-aware UTC
// datetime.
func UTCNowISO8601() string {
	return time.Now().UTC().Truncate(time.Second).Format("2006-01-02T15:04:05-07:00")
}

// FileSHA256 computes the SHA-256 hexdigest of path using streaming 1 MiB
// chunks, matching aiutils.file_utils.sha256.
func FileSHA256(path string) (string, error) {
	f, err := os.Open(path) // #nosec G304 -- operator-supplied --source path.
	if err != nil {
		return "", fmt.Errorf("hash source: %w", err)
	}
	defer func() { _ = f.Close() }()

	h := sha256.New()
	if _, err := io.CopyBuffer(h, f, make([]byte, 1<<20)); err != nil {
		return "", fmt.Errorf("hash source: %w", err)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
