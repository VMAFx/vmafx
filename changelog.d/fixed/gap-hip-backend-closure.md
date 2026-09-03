### Fixed
- **HIP backend**: resolved the HIP bucket of the backend-gap inventory ([ADR-1154](docs/adr/1154-hip-backend-gaps.md)):
  - Promoted 11 feature extractors (`integer_cambi_hip`, `ciede_hip`, `integer_psnr_hip`, `float_psnr_hip`, `float_moment_hip`, `integer_motion_v2_hip`, `float_motion_hip`, `float_ssim_hip`, `integer_ms_ssim_hip`, `integer_psnr_hvs_hip`, `float_adm_hip`) to active GPU execution with `VMAF_FEATURE_EXTRACTOR_HIP` / `TEMPORAL` flags, bringing active HIP extractors to 17/19, all passing parity tests on AMD hardware.
  - Fixed pointer-to-pointer kernel parameter packaging in `float_psnr_hip` and `float_moment_hip` (`&partials_dev`, `&sums_dev`).
  - Fixed parameter ordering in `float_moment_hip` kernel launch.
  - Fixed parameter types (`double c1..c3`) and partial buffer sizes (`sizeof(double)`) in `integer_ms_ssim_hip` to match `ms_ssim_vert_lcs` kernel signature.
  - Fixed option dictionary serialization in `integer_cambi_hip` by capturing options before dimension defaults.
  - Fixed chroma plane copying in `integer_psnr_hip` across all planes.
  - Added informative `vmaf_log` naming `-Denable_hipcc=true` before `-ENOSYS` returns in `float_ssim_hip.c`, `integer_ssim_hip.c`, and `vmaf_hip_picture_alloc`.
  - Pruned dead uncompiled `core/src/feature/hip/integer_adm/adm_decouple.hip` and orphan `integer_moment_hip.h` / `moment_score.hip`.
  - Implemented `vmaf_hip_dispatch_supports()` in `core/src/hip/dispatch_strategy.c` with `g_hip_features` lookup table and `VMAF_HIP_DISPATCH` env support.
  - Updated `docs/backends/hip/overview.md` to remove references to deleted stubs and document zero-copy DMA-BUF architectural limitations.
  - Formally deferred `integer_ssim_hip` (due to float kernel divergence vs CPU ground truth per ADR-0564) and `integer_adm_hip` (due to missing host-to-device picture staging buffers) in `docs/state.md`.

  Test-harness note: a skip now reports skip, not pass. `core/test/test.h`
  gained `mu_skipped`, and `test.c`'s `main()` exits 77 -- meson's "skipped"
  status -- when a test sets it. The four HIP parity tests that ADR-1154
  defers are registered `should_fail : true` because they fail on real AMD
  hardware, but they emit `[skip: no HIP device]` and exited 0 on a runner
  without one, so meson reported UNEXPECTEDPASS and the whole
  "Build — Linux (Intel LLVM, all backends)" leg failed while printing
  `Fail: 0`. Deleting those `should_fail` markers would have hidden a real
  deferred defect on capable hardware; making the skip path exit 77 keeps
  "could not run" and "ran and passed" distinct instead. Verified against a
  local HIP build: three tests SKIP and `test_hip_ssim_parity` reports
  EXPECTEDFAIL, so the suite exits 0 while the deferral stays honest. Every
  `[skip: ...]` site in those four sources sets the flag, including the CUDA
  ones in the two shared ADM sources.

  `vmaf_hip_dispatch_supports()` also gained three invariant assertions
  (Power of 10 rule 5, `scripts/ci/assertion-density.sh`): the strategy index
  a successful env parse returns must lie inside the table it was given, and
  the feature table must be non-empty. Both failure modes would silently
  mis-dispatch rather than fault -- an out-of-range index would enable HIP
  dispatch an operator asked to disable, and a truncated table would report
  every feature unsupported.
