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
