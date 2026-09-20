package p

// A cycle through methods is still a gap. Resolving a method call needs the receiver's type,
// and guessing it would invent edges that do not exist, so the call graph carries plain
// functions only. Reporting this would need type information the line-and-AST scanner has not
// got.
type walker struct{ depth int }

func (w *walker) descend() int {
	if w.depth <= 0 {
		return 0
	}
	w.depth--
	return w.ascend()
}

func (w *walker) ascend() int {
	if w.depth <= 0 {
		return 1
	}
	return w.descend()
}
