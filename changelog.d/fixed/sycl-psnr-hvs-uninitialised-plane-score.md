- **`Tidy SYCL` went green, and the thing blocking it was worth finding.**
  The gate fails when clang-tidy exits non-zero on any changed SYCL TU,
  which happens on the `WarningsAsErrors` set — and
  `integer_psnr_hvs_sycl.cpp` was tripping
  `clang-analyzer-core.UndefinedBinaryOperatorResult`: *"the right
  operand of `*` is a garbage value"*. `collect_fex_sycl()` fills
  `plane_score[0, n_active_planes)` but the combined-score expression
  reads indices 1 and 2 whenever `n_active_planes != 1`. The two agree
  today, because `n_active_planes` is assigned exactly 1 or
  `PSNR_HVS_NUM_PLANES`, so this was not a live defect — but nothing in
  the type says so, which is why the analyser could not rule out an
  undefined read. The array is now zero-initialised: no score changes
  while the invariant holds, and the read is defined if it is ever
  broken.
  With that and the preceding lint passes, all 23 SYCL translation units
  exit clean, so the gate passes rather than being tolerated as
  advisory.
