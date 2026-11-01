- **Tests compare two computed floating-point results by their bits, never with `==` or `!=`.**
  The six CodeQL `cpp/equality-on-floats` findings in `test_read_pictures_convert.c` and 39
  comparisons of the same shape in the CUDA, HIP, SYCL, SIMD and NEON twin tests now use
  `core/test/float_bits.h`, so a `+0` / `-0` mismatch no longer passes a bit-exact check. A new
  Semgrep rule refuses the shape in `core/test/` before a commit lands.
