// SPDX-License-Identifier: EUPL-1.2
package p

// Discarding a non-call value cannot discard an error.
func F() {
	_ = 1
}
