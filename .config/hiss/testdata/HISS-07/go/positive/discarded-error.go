package p

import "os"

// The error from Remove is discarded, which HISS-07 forbids.
func K() {
	_ = os.Remove("x")
}
