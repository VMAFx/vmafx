package p

// Discarding a non-call value cannot discard an error.
func F() {
	_ = 1
}
