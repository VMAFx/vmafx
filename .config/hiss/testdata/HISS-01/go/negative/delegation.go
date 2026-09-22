package p

import "io"

// W forwards to a same-named method on another value. Delegation is not recursion.
type W struct{ inner io.Closer }

func (w *W) Close() error {
	return w.inner.Close()
}
