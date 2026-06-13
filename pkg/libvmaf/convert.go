// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package libvmaf

import "math"

// Integer-width conversion helpers for the gRPC / node serving layers.
//
// The streaming RPCs surface frame indices and counts as Go `int` (FrameResult.Index,
// StreamResult.FramesProcessed) and elapsed times as `int64` milliseconds, but the
// generated proto messages use fixed-width unsigned fields (uint32 / uint64). A bare
// `uint32(n)` / `uint64(n)` conversion is flagged by gosec G115 (CWE-190) because a
// negative or oversized source silently wraps. These helpers saturate instead of
// wrapping: a negative input clamps to 0 and an over-range input clamps to the type
// maximum, so a malformed count can never masquerade as a wildly different value on the
// wire. All call sites pass values that are provably non-negative and in range (frame
// counts, monotonic indices, elapsed durations), so the clamps are defensive rather
// than load-bearing — but they make the conversion total and satisfy the analyzer
// without a blanket //nosec suppression.

// SafeUint32 converts a Go int to uint32, saturating out-of-range inputs to [0, MaxUint32]
// rather than wrapping. Negative inputs return 0; inputs above math.MaxUint32 return
// math.MaxUint32.
func SafeUint32(n int) uint32 {
	if n < 0 {
		return 0
	}
	if n > math.MaxUint32 {
		return math.MaxUint32
	}
	return uint32(n)
}

// SafeUint64 converts a Go int64 to uint64, clamping negative inputs to 0. Non-negative
// int64 values always fit in uint64, so no upper clamp is needed.
func SafeUint64(n int64) uint64 {
	if n < 0 {
		return 0
	}
	return uint64(n)
}
