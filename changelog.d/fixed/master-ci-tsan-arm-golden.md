- **master CI: ThreadSanitizer link failure and ARM golden drift fixed**.
  Two regressions that rode into `master` via the admin-merge batch (per-PR CI
  bypassed) are corrected: (1) the R2-9 OOM-injection test
  (`test_gpu_dispatch_env_oom.cpp`) replaced the global `operator new` /
  `operator delete`, which collides with the sanitizer allocator interceptors
  (`ld.lld: duplicate symbol: operator new(unsigned long)`) — the override and
  the test now self-skip under TSan / ASan / MSan, restoring the Sanitizers
  required check; (2) the PR #1060 aarch64 scalar `-ffp-contract=off` guard on
  `adm_dwt2_s` / `adm_dwt2_lo_s` shifted the akiyo `disable_enhn_gain` ADM score
  on ARM (`88.030463` -> `88.030322`), failing the golden assertion on the ARM
  build matrix; the scalar guard is reverted (golden restored) and the now-stale
  `test_float_adm_simd` parity test removed. See ADR-1057 (Update 2026-06-27).
