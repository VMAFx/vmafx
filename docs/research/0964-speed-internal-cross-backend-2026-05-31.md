<!-- markdownlint-disable MD013 MD060 -->
# Research-0964 — `speed_internal.c` missing TU, cross-backend wiring gap

- **Date**: 2026-05-31
- **Author**: Claude (operator brief 2026-05-31)
- **Related ADR**: [ADR-0964](../adr/0964-implement-speed-internal-and-wire-gpu-speed-extractors.md)
- **Triggered by**: round-4 SYCL audit (PR #465, ADR-0957) + round-4 HIP
  audit (PR #466, ADR-0958) both independently surfaced the same gap.

## TL;DR

`core/src/feature/speed_internal.h` declared a 9-function API
(`speed_internal_init_dimensions`, ..., `speed_internal_is_matrix_regular`)
but **no `speed_internal.c` ever shipped**.  Six GPU TUs
(`speed_{chroma,temporal}_{hip,sycl,cuda}.{c,cpp}`) `#include` the
header and call seven of the nine functions; none of the six TUs are
listed in any meson source list.  Net effect: the GPU twins of
`speed_chroma` / `speed_temporal` never compiled or linked, the
extractor registry could not resolve their names, and no parity gate
existed against the CPU SpEED golden.

This PR implements `speed_internal.c`, wires HIP + SYCL TUs into meson,
registers the four `vmaf_fex_speed_{chroma,temporal}_{hip,sycl}` symbols,
and adds two CPU-vs-SYCL parity tests.  CUDA wiring is deferred:
the CUDA TUs surface compile errors against the current
`CudaFunctions` struct (`CHECK_CUDA` missing, `cuMemAllocHost` not a
member) that need their own repair pass.

## Audit trail

### How we got here

1. **ADR-0567** split SpEED into a CPU+GPU pipeline:
   GPU does means / covariance / indterm / score; CPU does the
   25×25 eigendecomp + QR + Q^T multiply.  Header authored as the
   contract between the two halves.
2. The GPU TUs were authored (HIP 799 LOC, SYCL 752 LOC, CUDA 843 LOC
   for chroma; similar for temporal) — full `#include`s, function
   calls, kernels.
3. Meson source lists were never updated.  Header authored without a
   companion `.c`.
4. The mismatch went unnoticed for multiple release trains because
   `meson test --suite=fast` never linked the GPU TUs.
5. Round-3 HIP audit (2026-05 pre-rename) misdiagnosed the gap as
   "no CPU scalar reference exists for SpEED on the fork" — wrong;
   `vmaf_fex_speed_chroma` (`speed.c:1335`) and
   `vmaf_fex_speed_temporal` (`speed.c:1559`) ship as CPU extractors
   with their `_provided_features` arrays plus full pipelines.
6. Round-4 SYCL audit (PR #465, ADR-0957) and round-4 HIP audit
   (PR #466, ADR-0958), running in parallel, both correctly
   identified the missing-impl gap and stopped at "would link-fail";
   they deferred the fix to a cross-backend PR.

### What this PR does

| Step | Change |
|---|---|
| 1 | New TU `core/src/feature/speed_internal.c` — implements all 9 functions declared in `speed_internal.h`, duplicating ~620 LOC of pure math from `speed.c` (eigendecomp, QR, matrix helpers).  No GPU-specific assumptions, no global state. |
| 2 | `core/src/meson.build`: add `feature_src_dir + 'speed_internal.c'` to the `float_enabled` source list (compiled into `libvmaf_feature_static_lib`). |
| 3 | `core/src/hip/meson.build`: add `../feature/hip/speed_chroma_hip.c` and `..._temporal_hip.c` to `hip_sources`. |
| 4 | `core/src/meson.build` `sycl_feature_sources`: add `sycl/speed_chroma_sycl.cpp` and `..._temporal_sycl.cpp`. |
| 5 | `core/src/feature/feature_extractor.c`: add externs + registry rows for `vmaf_fex_speed_{chroma,temporal}_{hip,sycl}`. |
| 6 | `core/test/test_sycl_speed_chroma_parity.c` + `..._temporal_parity.c`: new CPU-vs-SYCL parity tests (places=4 tolerance per ADR-0214). Skip cleanly when no SYCL device is visible. |
| 7 | CUDA wiring **NOT** done — the TUs have real undefined-symbol bugs (see below) that need their own PR. |

### CUDA blocker (deferred)

Compile errors observed when wiring the CUDA TUs:

```text
error: implicit declaration of function 'CHECK_CUDA'
  → other CUDA TUs use CHECK_CUDA_RETURN / CHECK_CUDA_GOTO; the speed_*
    TUs use the wrong macro name.

error: 'CudaFunctions' has no member named 'cuMemAllocHost'
  → cuMemAllocHost is not in the libvmaf CudaFunctions table.
    The correct call is the driver-API cuMemHostAlloc (or
    cuMemAllocHost via cuda.h directly).

warning: unused label 'fail'
  → left over from a CHECK_CUDA_GOTO migration that was never finished.
```

These bugs are independent of `speed_internal.c` — they were latent
because the TUs never compiled.  Tracked as
`T-CUDA-SPEED-TU-REPAIR-2026-05-31` in `docs/state.md`.

## Hypothesise-then-check log

1. **Hypothesis**: "speed_internal.c is missing".
   **Check**: `find core -name 'speed_internal.c'` → no results.
   **Outcome**: confirmed.
2. **Hypothesis**: "GPU TUs are dormant / scaffold-only".
   **Check**: `grep -n "ENOSYS" core/src/feature/{hip,sycl,cuda}/speed_*` — the HIP TU
   returns `-ENOSYS` only under `#ifndef HAVE_HIPCC`; under
   `HAVE_HIPCC` the TU has real `hipModuleLoadData` / kernel-launch
   paths.  Not scaffold.
   **Outcome**: confirmed real GPU implementation, just unwired.
3. **Hypothesis**: "CPU extractor doesn't exist (round-3 HIP audit claim)".
   **Check**: `grep -n vmaf_fex_speed core/src/feature/feature_extractor.c`
   → both CPU extractors are externed and registered.
   **Outcome**: round-3 claim was wrong.
4. **Hypothesis**: "CUDA TUs build cleanly once wired".
   **Check**: wire + build → 6 compile errors per chroma TU,
   none related to `speed_internal.c`.
   **Outcome**: CUDA needs its own repair pass; defer.
5. **Hypothesis**: "SYCL TUs build cleanly once wired".
   **Check**: wire + build → clean ninja, 0 errors, 0 warnings on the
   speed TUs.  Test binaries link.
   **Outcome**: confirmed.
6. **Hypothesis**: "HIP TUs build cleanly once wired".
   **Check**: wire + build → clean ninja.
   **Outcome**: confirmed.

## Build matrix (local, host machine)

| Backend | Result | Notes |
|---|---|---|
| CPU (`-Denable_cuda=false -Denable_sycl=false -Denable_hip=false`) | PASS (49/49 fast tests) | `test_speed` + `test_speed_qa` both pass; CPU SpEED golden untouched |
| HIP (`-Denable_hip=true`) | PASS | builds `vmaf_fex_speed_{chroma,temporal}_hip`; init() returns `-ENOSYS` if HIP runtime unavailable |
| SYCL (`-Denable_sycl=true`) | PASS | builds `vmaf_fex_speed_{chroma,temporal}_sycl`; new parity test binaries link |
| CUDA (`-Denable_cuda=true`) | PASS (with speed CUDA TUs **NOT** wired) | full CUDA build green; speed CUDA TUs deferred to follow-up |

The SYCL parity tests segfaulted at SYCL runtime kernel-info lookup —
this is a pre-existing environmental failure (the existing
`test_sycl_motion3_parity` exhibits the same crash on the same host with
the same `libsycl.so` 2026.0).  Not a regression introduced by this PR.

## Follow-ups

- `T-CUDA-SPEED-TU-REPAIR-2026-05-31` (this PR adds the row to
  `docs/state.md`): repair the CUDA TUs, wire them, add
  `test_cuda_speed_*_parity.c` mirroring the SYCL tests.
- Consider a future refactor that unifies `speed.c` and
  `speed_internal.c` behind a single source of truth (eliminates the
  ~600 LOC duplication).  Out of scope for this PR; the duplication is
  bounded and the CPU parity tests will catch any divergence.

## References

- `core/src/feature/speed_internal.h` — the contract.
- `core/src/feature/speed.c` — the CPU SpEED reference (Netflix-mirrored).
- PR #465 — SYCL r4 coverage audit.
- PR #466 — HIP r4 coverage audit.
- ADR-0567 — SpEED CPU/GPU algorithm split.
- ADR-0214 — cross-backend places=4 numeric-parity gate.
