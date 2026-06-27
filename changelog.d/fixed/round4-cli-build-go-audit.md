- **Round-4 audit bug-fix bundle (CLI / build / Go-MCP)**. Five verified defects
  from the adversarial audit of the admin-merged #1043–1062 batch: (1) the
  `x86_float_adm_avx2` / `x86_float_adm_avx512` meson carve-outs were missing
  `_x86_simd_strict_fp_extra`, so under Intel `icx` the float-ADM AVX2/512 paths
  could FMA-contract differently from the scalar reference (the guard is a no-op
  on gcc/clang and golden-neutral there; it only forces `-fp-model=precise` on
  icx, matching the `ssim`/`ssimulacra2` carve-outs). (2) `vmaf.cpp` `wall_time_s`
  read an uninitialised `struct timespec` if `clock_gettime` failed (UB); now
  zero-initialised. (3) the Windows `wall_time_s` / `vmaf_bench.c` `now_ms`
  re-queried `QueryPerformanceFrequency` every call and could divide by zero on
  failure; the frequency is now cached (`static`, query-once) with a
  zero-frequency guard, and the previously-unchecked `QueryPerformance*` returns
  are `(void)`-cast. (4) corrected a stale `libvmaf/tools/vmaf.c` →
  `core/tools/vmaf.cpp` comment in `core/tools/meson.build` (ADR-0700 +
  ADR-0809). (5) **Go MCP path allowlist hardened**: `AllowedRoots()` used
  `RepoRoot()`'s cwd fallback, so running the server outside the repo
  allowlisted arbitrary `<cwd>/testdata` / `python/test/resource` / `model`
  trees; it now fails closed via an unexported `discoverRepoRoot()` that only
  contributes the repo-relative roots when the `CLAUDE.md` marker is actually
  found (the container mount + `VMAF_MCP_ALLOW` remain unconditional), mirroring
  the C `discover_repo_root()` guard. `RepoRoot()`'s contract is unchanged for
  path-joining callers.
