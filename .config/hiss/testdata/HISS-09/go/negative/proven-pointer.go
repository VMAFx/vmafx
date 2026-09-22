package p

import "unsafe"

// First reinterprets the slice header under a stated proof.
func First(b []byte) *byte {
	// SAFETY: every caller checks len(b) > 0, so &b[0] is within the allocation.
	return (*byte)(unsafe.Pointer(&b[0]))
}
