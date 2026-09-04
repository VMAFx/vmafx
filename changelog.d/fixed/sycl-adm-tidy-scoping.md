# fix(sycl): clean up integer_adm_sycl warnings and scope CI codegen step

Fix real compiler warnings in `core/src/feature/sycl/integer_adm_sycl.cpp` reported by
`icpx` during the advisory SYCL clang-tidy workflow:
- Reordered designated initializers in `static const VmafOption options[]` so `.help` precedes
  `.alias`, matching declaration order in `struct VmafOption` (`opt.h`) and satisfying ISO C++.
- Removed duplicate `const` specifiers from `ref_ptrs` and `dis_ptrs` (`const int32_t *const`),
  and eliminated obsolete `misc-const-correctness` NOLINT comments.
- Guarded shift calculations in `launch_decouple_csf` and `launch_csf_den_cm_3band` against non-positive
  shift counts (`ks = (17 - clz > 0) ? (17 - clz) : 1`), resolving static analyzer reports.

Fix the CI scoping defect in `.github/workflows/lint-and-format.yml`:
- Replaced whole-tree `meson compile -C build-sycl` in the `Generate SYCL compile_commands.json`
  step with targeted `ninja -C build-sycl include/vcs_version.h`. Previously, the unconstrained
  compile built all 1,413 targets with `icpx`, causing warnings in untouched SYCL files to be
  emitted and captured by GitHub Actions' gcc problem matcher on unrelated PRs.
- Updated `scripts/ci/gen-sycl-compile-commands.py` to strip `icpx`-specific arguments
  (`-fsycl-targets`, `-Xs`, `-fp-model`) and idempotently refresh SYCL TU entries in `compile_commands.json`.
- Updated `scripts/ci/clang-tidy-sycl.sh` to remove `-D__SYCL_DEVICE_ONLY__=0` (which erroneously
  tripped `#ifdef __SYCL_DEVICE_ONLY__` and broke SPIR-V header declarations) and ensure `-std=c++20`
  is passed for SYCL translation units.
