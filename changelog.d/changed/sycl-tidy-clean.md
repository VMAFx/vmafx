
- **The 24 SYCL sources and parity tests the train touches are clang-tidy
  clean.** ADR-0141's touched-file rule applies to them, and the advisory
  `Tidy SYCL` lane was reporting 512 findings across them. Most were mechanical
  and are now fixed rather than suppressed: 22 signed/unsigned comparisons use
  `std::cmp_greater_equal` like `integer_adm_sycl.cpp` already did, 30 unbraced
  statements gained braces, 15 multiplications that were widened after the fact
  now compute in `size_t`, 11 multi-declarations are one per line, and the
  `VmafOption` terminators use designated initialisers. The 390
  `modernize-use-nullptr` findings in the C parity tests take the cited
  `NOLINTBEGIN` band that `core/test/test_picture.c` established, because
  MSVC's `/std:clatest` has no C23 `nullptr` and those TUs build on the Windows
  lanes (ADR-1138). The `misc-use-anonymous-namespace` findings take the band
  `integer_motion_sycl.cpp` and `integer_adm_sycl.cpp` already carry: the
  entry points live inside `extern "C"` because their addresses populate a
  C-ABI dispatch struct, and a namespace cannot appear in a linkage
  specification at all.
