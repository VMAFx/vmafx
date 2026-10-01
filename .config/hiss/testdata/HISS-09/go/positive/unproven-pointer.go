// SPDX-License-Identifier: EUPL-1.2
package p

import "unsafe"

// First reinterprets the slice header without a proof.
func First(b []byte) *byte {
	return (*byte)(unsafe.Pointer(&b[0]))
}
