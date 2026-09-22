fn first(p: *const u8) -> u8 {
    // SAFETY: p is non-null, aligned and points at an initialised byte.
    unsafe { *p }
}
