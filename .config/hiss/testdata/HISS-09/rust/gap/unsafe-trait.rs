// Declaring an unsafe trait places an obligation on every implementor, which is the
// invariant a SAFETY proof exists to state. Neither `unsafe {` nor the `unsafe fn`
// prefix test matches this line.
unsafe trait Contiguous {
    fn len(&self) -> usize;
}
