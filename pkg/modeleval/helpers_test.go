// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package modeleval

import (
	"fmt"
	"os"
)

// keyFor builds a deterministic clip key for split-distribution tests.
func keyFor(i int) string { return fmt.Sprintf("clip_%06d", i) }

// writeBytes writes b to path, for the malformed-input tests.
func writeBytes(path string, b []byte) error {
	return os.WriteFile(path, b, 0o600)
}
