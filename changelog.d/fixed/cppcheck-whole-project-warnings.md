Fix all Cppcheck (Whole Project) CI warnings on master:

- `integer_ssim.c` — add null-check after `malloc` in `gaussian_filter_init` (returns
  `-ENOMEM`), and guard `lines`/`line_buf` allocations in `calc_ssim` with early-exit
  paths that free already-allocated memory before returning.
- `picture_pool.cpp` — pass `VmafPicturePoolConfig` by `const &` in the static helper
  `pool_preallocate_pictures` (removes a struct copy on every call).
- `read_json_model.cpp` — replace `memset(m, 0, …)` with `*m = VmafModel{}` to avoid
  the `memsetClassFloat` portability warning; fix two `%d` format specifiers that
  accepted `unsigned int` to `%u`.
- `vmaf.cpp` — fix two `%d` format specifiers that accepted `unsigned int` to `%u`.
- `test_ssimulacra2_simd.c` — add null-checks after `calloc` in `test_host_xyb` and
  `test_host_downsample`; fail the test immediately rather than dereferencing NULL.
- `test_tensor_io.c` — initialize `src[4]` to `{0}` (was uninitialised stack memory).
- `.cppcheck-suppressions.txt` — add `invalidPointerCast` entry for `opt.cpp` to match
  the existing entry for `opt.c` (same intentional cast by design, ADR-0772).
