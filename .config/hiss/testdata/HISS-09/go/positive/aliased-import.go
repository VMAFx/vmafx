package p

import us "unsafe"

// First does exactly what the positive fixture does, through a renamed import. The
// check requires the selector's package identifier to be spelled "unsafe", so an
// import alias removes the finding without changing the behaviour.
func First(b []byte) *byte {
	return (*byte)(us.Pointer(&b[0]))
}
