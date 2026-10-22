## Hybrid GCC CPU and icpx SYCL toolchain (2026-10-19)

- `core/src/meson.build` keeps the `BEGIN/END VMAF C++ strict FP policy for
  mixed toolchains` block above the first build target, with the ADR-2170
  icx spelling (`-fp-model=precise -fno-fast-math -fcomplex-arithmetic=full
  -ffp-contract=off`); `core/test/test_strict_fp_compiler_args.py` executes
  it for every C and C++ compiler pair.
- Every `executable()` in `core/test/meson.build` passes
  `kwargs : test_link_kwargs` or sets `link_language` itself, so a C test
  links through icpx when SYCL is on;
  `core/test/test_test_link_kwargs_contract.py` refuses one that does not.
  Fork-only, nothing upstream.
