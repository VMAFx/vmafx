## SYCL SpEED covariance is the reference's sequential fp64 sum (2026-10-08)

`fix/sycl-speed-covariance-exact`,
[ADR-2690](adr/2690-sycl-speed-covariance-fast-exact.md). SYCL kernel, one new
header, tests, docs.

- `speed_sycl_pipeline.cpp` loses the pair-sum covariance kernel
  (`centred_product`, `accumulate`, `covariance_partial`, `covariance_group`,
  `kGroup`). `launch_covariance()` runs the helpers of
  `sycl_speed_cov_math.h` in three launches: differences, products (both
  stored as fp64 bit patterns in the new `cov_diff` / `cov_terms` buffers,
  `cov_sums` carries the running sum between row slices) and one sequential
  add chain per (channel, entry); `covariance_entry()` is the same operations
  in one function, kept for the device probe. Upstream Netflix
  has no SYCL twin, so a sync never conflicts here; a rebase onto a branch
  that still has the old kernel keeps this side. `speed.c` is untouched.
- The covariance quotient is `signed_div()` by the exact `uint64_t` count,
  which keeps the exact-count division of #2188; `count_ff()` and
  `ff_div_to_float()` stay in `sycl_exact_fp.h` but no longer store a
  covariance. `test_speed_cov_count_contract.py` holds SYCL to the soft-fp64
  quotient and refuses a pair store in the pipeline.
- `core/test/test_sycl_speed_cov_math`, `test_sycl_speed_cov_chain`
  (device-free, with `speed_cov_cases.h` and the synthetic plane
  `speed_cov_synthetic_plane.h`, written by
  `scripts/dev/gen_speed_cov_synthetic_plane.py`) and `test_sycl_speed_cov_exact_contract.py`
  are new. No rebase impact outside
  `core/src/feature/sycl/` and `core/test/`.
