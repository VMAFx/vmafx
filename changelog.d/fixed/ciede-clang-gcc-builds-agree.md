- **A clang build returns the `ciede2000` scores of a GCC build.** `ciede.c`
  writes `powf(degrees, 2)`. GCC calls the C library; clang replaced the call
  by a product, the correctly rounded square, and glibc's `powf` returns the
  other neighbouring `float` on about 0.12 % of the arguments. The two builds
  differed on 65 of 180 measured frames (the Netflix 576x324 pair at 8 to 16
  bits and as 10-bit 4:2:2, Sparks, both 1920x1080 checkerboard pairs, Big
  Buck Bunny at 1920x1080 and 3840x2160), by at most 2.0e-11, on x86-64 and
  on aarch64. `ciede.c` is now built in a library of its own with
  `-fno-builtin-powf` under clang
  ([ADR-1467](docs/adr/1467-ciede-powf-library-call.md)), and the two builds
  agree on all 180 frames on both architectures. A GCC build is unchanged:
  the machine code of `ciede.c` and every measured report are identical. An
  icx build is left as it is (it links Intel's math library). The new
  `test_ciede_powf_call` compares `get_r_sub_t()` with the same expression
  called through a function pointer over 5.2 million values, and
  `test_ciede_device_math` now runs on aarch64 too.
