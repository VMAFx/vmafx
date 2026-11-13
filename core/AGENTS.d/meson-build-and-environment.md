---
paths:
  - core/meson.build
  - scripts/ci/run_meson_test.py
invariant: Meson strips secret tokens from test environment, pins version >= 1.4.0, and tracks SYCL headers.
area: build-and-toolchains
---
<!-- markdownlint-disable MD013 MD060 -->
# Meson build configuration, dependency tracking, and environment sanitization

## Rebase-sensitive invariants

- **Meson test secret environment sanitization**
  ([ADR-1333](../../docs/adr/1333-meson-test-secret-env-sanitization.md);
  [Research-1333](../../docs/research/1333-meson-test-secret-env-sanitization.md)):
  `scripts/ci/run_meson_test.py` deletes sensitive GitHub credential keys before Meson
  records parent environment in `testlog.txt`; all Make, CI, preflight, bisection,
  setup-guidance, Zed callers must use it. `core/meson.build` retains same denylist in
  project-wide default test setup for (`GITHUB_PERSONAL_ACCESS_TOKEN`, `GITHUB_TOKEN`,
  `GH_TOKEN`, `GH_ENTERPRISE_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `GITHUB_PAT`, `GH_PAT`,
  `GITHUB_AUTH_TOKEN`, `GITHUB_API_TOKEN`, `HOMEBREW_GITHUB_API_TOKEN`,
  `ACTIONS_ID_TOKEN_REQUEST_TOKEN`, `ACTIONS_RUNTIME_TOKEN`) at child and JSON-log boundary.
  Meson applies per-test environments after setup; permits alternate setups. Regression
  contract inventories callers; rejects raw test-target bypasses; enumerates all
  `core/**/meson.build` files. Requires single `add_test_setup`; rejects explicit
  forbidden-name reintroduction outside twelve sanctioned unset calls. Rebase preserves
  runner, callers, setup, regression together. Direct raw external Meson/Ninja commands
  remain outside bounded guarantee.
  ([ADR-1320](../../docs/adr/1320-cuda-hip-kernel-header-dependency-tracking.md);
  [Research-2106](../../docs/research/2106-cuda-hip-kernel-header-dependency-tracking.md)):
  CUDA fatbin (`cu_ptx_target_*`) and HIP HSACO (`hip_hsaco_*`) custom targets in
  `core/src/meson.build` bind explicit header dependency lists
  (`depend_files: cuda_kernel_shared_headers` and `depend_files: hip_kernel_shared_headers`).
  Lists cover complete repo-local quoted include closure. Combined with compiler depfiles
  (`-MD -MF @DEPFILE@` on POSIX nvcc; `-Xclang -dependency-file -Xclang @DEPFILE@` on
  hipcc; depfile omitted on Windows MSVC). CUDA list also binds generated `config_h_target`.
  Shared kernel header changes (e.g., `integer_adm_cuda.h`, `vif_cuda.h`) trigger
  incremental device binary rebuilds without manual `touch` workarounds. Rebase preserves
  dependency declarations.
- **SYCL TU header dependency tracking** (ADR-1320 applied to SYCL,
  `T-SYCL-TU-HEADER-DEPS-UNTRACKED-2026-10-01`). `sycl_common_<name>` +
  `sycl_feature_<name>` custom targets in `core/src/meson.build` declare
  `depfile` + pass `sycl_depfile_args` (`-MD -MF @DEPFILE@`; empty on
  Windows, lane builds clean). New SYCL compile target -> same two lines.
  Without: header edit (`feature/sycl/sycl_*.h` = kernel arithmetic) leaves
  old kernels in the library, `ninja` says nothing to do. Guard:
  `core/test/test_device_target_header_dependencies.py`.
- **`meson_version` is pinned to `>= 1.4.0`, not upstream's value**
  (fork-local, ADR-0692 / T-CI-MESON-C23-APT-2026-08-30):
  [`meson.build`](../meson.build) sets Meson's built-in fallback list
  `c_std=c23,c2x,c17,none`. Meson selects first spelling supported by
  active compiler; MSVC-syntax drivers then receive `/std:clatest` as narrow
  override. Keep `none` last: Meson's `intel-llvm-cl` backend advertises only
  `c89`/`c99`/`c11`, so list without value every backend accepts aborts
  configure on Windows MSVC+SYCL leg.
  `c23` is only recognised from Meson 1.4.0 onward (verified: 1.3.2 rejects
  it, 1.4.0 accepts it). Declared `meson_version` must stay at or above 1.4.0
  for as long as fork keeps this standard policy. Upstream sync that
  rewrites `project()` will conflict here — keep fork's
  `>= 1.4.0`. Lowering it does not fail loudly: build instead dies
  much later at configure with cryptic
  `ERROR: Unknown C std ['c23']`, which is what took out seven CI
  workflows at once when Ubuntu's meson 1.3.2 was still in use.
- **Language standards are Meson built-in fallback lists** (ADR-1056,
  2026-06-04): `project()` owns `c_std=c23,c2x,c17,none` and
  `cpp_std=c++26,c++23,c++latest`. Do not restore manual `-std=` probing or
  injection: it bypasses Meson's compiler checks, duplicates flags, and emits
  configure-time warnings on current Meson. Callers may still override either
  built-in option. trailing `none` is not optional — it is only value
  every backend accepts, and `intel-llvm-cl` (icx-cl) reaches no other entry
  in list. only platform exception is MSVC-style C driver: after
  Meson selects `c17` (cl.exe) or `none` (icx-cl), `/std:clatest` is appended
  both to project arguments and feature probes so fork retains its
  newest-C contract.

- **Build-option combination validation** (fork-local, fixes 1b/1c/1d of audit-build-matrix-symbols-2026-05-16):
  `core/src/meson.build` validates dependent-option combinations, errors or warns when incompatible flags are set:
  — `enable_mcp_sse=enabled/true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_uds=true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_stdio=true` requires `enable_mcp=true` (error if violated);
  — `enable_avx512=true` with `enable_asm=false` issues warning (no-op, not error);
  — `enable_hipcc=true` with `enable_hip=false` issues warning (no-op, not error).
  Checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. Principle: every option depending on another must `error()` on bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](../src/meson.build).
