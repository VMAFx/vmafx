- **dev/Containerfile**: add `libclang-rt-19-dev` to the build-deps apt layer so
  that `b_sanitize=address,undefined` meson builds link correctly against the
  clang-19 ASan/UBSan runtime libraries (previously the linker failed with
  `cannot find libclang_rt.asan.a`).
- **vmaf-tune `--score-backend`**: remove `vulkan` from `ALL_BACKENDS` /
  `DEFAULT_FALLBACKS` in `score_backend.py` and from all argparse `choices=`
  tuples, following the Vulkan backend removal in ADR-0726. Requesting
  `--score-backend vulkan` now raises a `ValueError` at startup.
- **vmaf-tune docs**: update `docs/usage/vmaf-tune-score-backend.md` to reflect
  the `cuda → sycl → hip → cpu` fallback chain and remove the Vulkan row from
  the accepted-values table.
- **Go CI (`.github/workflows/go-ci.yml`)**: post-ADR-0700 sweep miss — the
  cgo-link build step ran `meson setup core/build-cpu` from repo root, which
  fails with `Neither source directory 'core/build-cpu' nor build directory
  None contain a build file meson.build.` since the rename moved `meson.build`
  into `core/`. Pass the source dir explicitly (`meson setup core/build-cpu
  core ...`). Restores Go CI green on master.
- **SYCL integer extractors (`core/src/feature/sycl/integer_adm_sycl.cpp`,
  `integer_vif_sycl.cpp`)**: add the missing `close_fex_sycl` forward
  declaration. The init-failure cleanup paths added by ADR-0784 (sycl-init-
  failure-cleanup-leaks) call `close_fex_sycl(fex)` from within
  `init_fex_sycl`, but the function is defined later in the file as
  `static int` without a forward decl, so SYCL builds fail with
  `error: use of undeclared identifier 'close_fex_sycl'`. Matches the pattern
  already used by `float_*_sycl.cpp`. Restores Linux GCC (all backends),
  Ubuntu SYCL, Ubuntu SYCL + CUDA, and macOS SYCL builds.
