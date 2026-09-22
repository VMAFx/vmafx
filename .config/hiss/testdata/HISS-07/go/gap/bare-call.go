package p

import "os"

// A bare call statement drops the error entirely. Deciding this needs type
// information the syntax scanner does not build; errcheck covers it in the same gate.
func M() {
	os.Remove("x")
}
