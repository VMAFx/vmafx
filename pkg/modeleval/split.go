// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/modeleval/split.go — deterministic train/val/test bucketing.
//
// This is a byte-faithful Go port of the SHA-256 key hashing that the
// Python MCP server inlines in `_eval_model_on_split`
// (mcp-server/vmaf-mcp/src/vmaf_mcp/server.py) and that the embedded
// helper script in cmd/vmafx-mcp/impl.go used to shell out to python3
// for. The scheme mirrors `vmaf_train`'s `split_keys` so a model
// evaluated through the MCP surface sees exactly the same rows it saw
// during training.
//
// The bucket function must stay bit-identical to the Python original:
//
//	h = hashlib.sha256(f"vmaf-train-splits-v1:{key}".encode()).digest()
//	bucket = int.from_bytes(h[:8], "big") / (1 << 64)
//
// Any drift here silently re-partitions every cached feature table and
// invalidates cross-language comparisons, so the constants below are
// load-bearing — see ADR-0704 (Go MCP port) for the parity contract.

package modeleval

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"
)

// splitSalt prefixes every key before hashing. Changing this string
// re-partitions every split; it is versioned ("v1") for exactly that
// reason and must match the Python side verbatim.
const splitSalt = "vmaf-train-splits-v1:"

// Split fractions. testFrac claims the first 10% of the bucket space and
// valFrac the next 10%; everything above testFrac+valFrac is train.
// Kept as separate named constants (rather than a folded 0.2) so the
// comparison chain is structurally identical to the Python original.
const (
	testFrac = 0.1
	valFrac  = 0.1
)

// Split names accepted by Evaluate. "all" disables filtering entirely.
const (
	SplitTrain = "train"
	SplitVal   = "val"
	SplitTest  = "test"
	SplitAll   = "all"
)

// ValidSplits lists the accepted split names in the same order as the
// Python `_VALID_SPLITS` tuple, so error messages match.
var ValidSplits = []string{SplitTrain, SplitVal, SplitTest, SplitAll}

// ValidateSplit reports whether name is an accepted split, returning an
// error whose text mirrors the Python server's ValueError.
func ValidateSplit(name string) error {
	for _, s := range ValidSplits {
		if name == s {
			return nil
		}
	}
	return fmt.Errorf("split must be one of ('train', 'val', 'test', 'all'); got %q", name)
}

// Bucket maps a key to a deterministic float in [0, 1).
//
// Port note: Python computes `int.from_bytes(h[:8], "big") / (1 << 64)`,
// an exact-integer division that the interpreter rounds once to the
// nearest float64. math.Ldexp(float64(u), -64) rounds u to float64 and
// then scales by a power of two, which is exact — so both languages land
// on the identical double for every input.
func Bucket(key string) float64 {
	sum := sha256.Sum256([]byte(splitSalt + key))
	u := binary.BigEndian.Uint64(sum[:8])
	return math.Ldexp(float64(u), -64)
}

// SplitOf returns the split a key belongs to: "test", "val" or "train".
// The comparison order matches the Python `which()` closure exactly.
func SplitOf(key string) string {
	b := Bucket(key)
	if b < testFrac {
		return SplitTest
	}
	if b < testFrac+valFrac {
		return SplitVal
	}
	return SplitTrain
}
