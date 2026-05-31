<!-- markdownlint-disable MD060 -->
# HIP kernel parity coverage — round 4 audit (2026-05-31)

Companion research digest for ADR-0958. Quantifies the residual
HIP-side coverage gap after PR #443 / ADR-0945 round-3, ranks the
remaining extractors by reachability, and justifies the 2-test
selection (plus the two deferrals — `speed_*_hip` blocked on a
pre-existing latent link defect, `float_moment_hip` blocked on a
CPU/HIP `provided_features` mismatch) that round-4 ships.

## Inventory

`find core/src/feature/hip -maxdepth 1 -name '*_hip.c'` returns 18
HIP-related TUs (17 real extractors + `hip_hsaco_stubs.c` build-only
fallbacks). The effective HIP extractor count is 17.

| Extractor source | Registered name | Tested before round-4? | Round-4 ships test? |
|---|---|---|---|
| `integer_adm_hip.c` | `adm_hip` | yes — `test_hip_adm_parity` (ADR-0539) | — |
| `integer_motion_v2_hip.c` | `motion_v2_hip` | yes — `test_hip_motion3_parity` | — |
| `integer_psnr_hip.c` | `psnr_hip` | yes — `test_hip_psnr_parity` (PR #351) | — |
| `integer_vif_hip.c` | `vif_hip` | yes — `test_hip_vif_parity` (PR #351) | — |
| `ciede_hip.c` | `ciede_hip` | yes — `test_hip_ciede_parity` (PR #372) | — |
| `integer_psnr_hvs_hip.c` | `psnr_hvs_hip` | yes — `test_hip_psnr_hvs_parity` (PR #372) | — |
| `integer_motion_hip.c` | `motion_hip` (v1) | yes — `test_hip_motion_parity` (PR #372) | — |
| `integer_ssim_hip.c` | `integer_ssim_hip` | yes — `test_hip_ssim_parity` (PR #372) | — |
| `integer_ms_ssim_hip.c` | `integer_ms_ssim_hip` | yes — `test_hip_ms_ssim_parity` (PR #372) | — |
| `integer_cambi_hip.c` | `cambi_hip` | yes — `test_hip_cambi_parity` (PR #443) | — |
| `float_adm_hip.c` | `float_adm_hip` | yes — `test_hip_float_adm_parity` (PR #443) | — |
| `float_motion_hip.c` | `float_motion_hip` | yes — `test_hip_float_motion_parity` (PR #443) | — |
| `float_psnr_hip.c` | `float_psnr_hip` | yes — `test_hip_float_psnr_parity` (PR #443) | — |
| `ssimulacra2_hip.c` | `ssimulacra2_hip` | **no** | **yes** — `test_hip_ssimulacra2_parity` |
| `float_ssim_hip.c` | `float_ssim_hip` | **no** | **yes** — `test_hip_float_ssim_parity` |
| `speed_chroma_hip.c` | `speed_chroma_hip` | **no** | **deferred** — pre-existing link defect |
| `speed_temporal_hip.c` | `speed_temporal_hip` | **no** | **deferred** — pre-existing link defect |
| `float_moment_hip.c` | `float_moment_hip` | **no** | **deferred** — `provided_features` mismatch |

Coverage delta: **13 / 17 → 15 / 17** parity-gated HIP extractors
(≈76% → ≈88%). Two carryover deferrals are tracked under new
`docs/state.md` rows.

## Selection rationale

The 2 round-4 picks were ranked on three axes:

1. **Reachability** — both selected kernels ship a public CPU twin
   with a matching `provided_features` key the parity gate asserts
   against:
   - `ssimulacra2` (CPU, `core/src/feature/ssimulacra2.c:1126`) ↔
     `ssimulacra2_hip` (HIP, `core/src/feature/hip/ssimulacra2_hip.c:1055`),
     both emit `{"ssimulacra2", NULL}`.
   - `float_ssim` (CPU, `core/src/feature/float_ssim.c:204`) ↔
     `float_ssim_hip` (HIP, `core/src/feature/hip/float_ssim_hip.c:648`),
     both emit `{"float_ssim", NULL}`.

2. **Round-3 deferral resolution check**. The round-3 audit
   (ADR-0945) deferred four kernels with these stated reasons:
   - "ssimulacra2_hip — PR #290 in flight on this file." That PR has
     merged; the file is no longer in active refactor. **Resolved →
     ships in round 4.**
   - "float_ssim_hip — mirror of integer SSIM already covered;
     deferred to round 4 to keep round 3 reviewable." Strictly a
     batching deferral. **Resolved → ships in round 4.**
   - "speed_*_hip — no CPU scalar reference exists for the
     speed-family features on master." **Incorrect**.
     `vmaf_fex_speed_chroma` (`core/src/feature/speed.c:1335`) and
     `vmaf_fex_speed_temporal` (line 1559) ship as stable extractors
     in `speed.c`; the Python compat port via
     T-SPEED-PYTHON-COMPAT-2026-05-28 also references them. The
     parity tests were drafted and verified to compile, but the
     **link** step surfaced a deeper defect (next bullet).
   - "float_moment_hip — internal helper, no public CPU twin
     surface." Partially correct. The CPU twin does ship
     (`vmaf_fex_float_moment` in `core/src/feature/float_moment.c`)
     but its `provided_features` is a single `{"float_moment", NULL}`
     channel whereas the HIP twin emits four per-stat keys. A parity
     gate against the mismatched surface has no shared LHS/RHS.

3. **Linkability** — verified by container build (vmaf-dev-mcp,
   `enable_hip=true enable_hipcc=false`):
   - `ssimulacra2_hip` and `float_ssim_hip` link cleanly; their TUs
     are already in `core/src/hip/meson.build`'s `hip_sources`.
   - `speed_chroma_hip` and `speed_temporal_hip` are **not** in
     `hip_sources`. Adding them surfaces 4 undefined references at
     link time:

     ```text
     undefined reference to `speed_internal_init_dimensions'
     undefined reference to `speed_internal_float_stride'
     ```

     These two helpers are declared in
     `core/src/feature/speed_internal.h` (lines 85, 93) but **no .c
     implementation exists** anywhere in `core/src/feature/`. The
     header's comment block claims the helpers live in `speed.c`,
     but a `grep` of `speed.c` returns zero hits. The CUDA twins
     (`speed_chroma_cuda.c:645`, `speed_temporal_cuda.c:443`) and
     SYCL twins reference the same helpers, but **none of the GPU
     speed-family TUs are currently wired into their respective
     meson archives** — so the bug is latent on all three GPU
     backends. Fixing it requires extracting the algorithm-defining
     logic into a new `core/src/feature/speed_internal.c`, plus
     unit tests for that file. That work is non-test-shaped and out
     of scope for a parity-coverage PR.

## Fixture choices

| Test | Geometry | Bit depth | Frames | Tolerance | Rationale |
|---|---|---|---|---|---|
| `test_hip_ssimulacra2_parity` | 256×144 | 8 | 1 | 1e-3 | >= 8×8 lower bound (line 851); 6-scale pyramid rounding ≈ MS-SSIM |
| `test_hip_float_ssim_parity` | 256×144 | 8 | 1 | 1e-3 | >= 8×8 lower bound; mirrors integer SSIM round-2 places=3 budget |

All fixtures use synthetic gradients
(`(row + col + salt * k) & 0xFF` for the luma plane; constant `128u`
for U/V).

## Skip-path verification

Each test exercises the same skip contract as the round-3 tests:

1. `vmaf_hip_state_init()` → `[skip: no HIP device]` on hosts
   without an AMD GPU or without the HIP runtime resolvable at
   load time.
2. `vmaf_use_feature(..._hip) == -ENOSYS` → `[skip: HIP scaffold
   ENOSYS]` on hosts that built HIP support without
   `enable_hipcc=true` (the `#ifndef HAVE_HIPCC` guard returns
   ENOSYS).
3. `feed_frame() / vmaf_read_pictures() == -ENOSYS` → `[skip: HIP
   scaffold ENOSYS on feed / EOS]` for any extractor that defers
   the ENOSYS return to submit/collect time.

Container-verified (vmaf-dev-mcp:cuda13.3, `enable_hip=true
enable_hipcc=false`):

```text
==== test_hip_ssimulacra2_parity ====
test_ssimulacra2_hip_registered: pass
test_ssimulacra2_cpu_hip_parity: [skip: no HIP device] pass
2 tests run, 2 passed
exit=0
==== test_hip_float_ssim_parity ====
test_float_ssim_hip_registered: pass
test_float_ssim_cpu_hip_parity: [skip: no HIP device] pass
2 tests run, 2 passed
exit=0
```

## Tolerance basis

Both tests use places=3 (1e-3) per ADR-0214's "filtered features"
budget:

- `ssimulacra2` — 6-scale Gaussian pyramid with SSIM-like
  multiplicative pooling; more aggressive than MS-SSIM, places=3
  is the floor.
- `float_ssim` — same algorithmic shape as integer SSIM (round-2
  places=3 precedent, ADR-0883).

A tighter tolerance (places=4) was rejected as likely flaky on
AMD GPU hardware where the multi-scale / windowed accumulations
diverge from CPU IEEE-754 ordering.

## Open follow-ups

- **T-HIP-SPEED-INTERNAL-IMPL-MISSING-2026-05-31** (new, added to
  `docs/state.md` Open bugs):
  `speed_internal_init_dimensions` and `speed_internal_float_stride`
  declared in `speed_internal.h` but never defined. Blocks
  `speed_*_hip`, `speed_*_cuda`, and `speed_*_sycl` from linking.
  Fix is to extract algorithm-defining logic from `speed.c` into a
  new `core/src/feature/speed_internal.c`, plus a host-only unit
  test (`test_speed_internal.c`) covering the resolution math +
  alignment edge cases. Once landed, the speed-family parity gates
  can ship in a focused round-5 PR.
- **T-HIP-FLOAT-MOMENT-PROVIDED-FEATURES-MISMATCH-2026-05-31**
  (added to `docs/state.md` deferred): CPU emits `float_moment`,
  HIP emits 4 per-stat keys. Needs an API-shape decision before a
  parity gate is meaningful.

## References

- [ADR-0958](../adr/0958-hip-kernel-coverage-round4.md) — this
  round's decision record.
- Round 1: [ADR-0868](../adr/0868-hip-kernel-coverage-round1.md),
  PR #351.
- Round 2: [ADR-0883](../adr/0883-hip-kernel-coverage-round2.md),
  PR #372.
- Round 3: [ADR-0945](../adr/0945-hip-kernel-coverage-round3.md),
  PR #443.
- Backend tolerance policy:
  [ADR-0214](../adr/0214-gpu-numerical-tolerance.md).
- HIP backend audit motivating the rounds: Research-0755,
  `docs/research/0755-hip-backend-audit-20260529.md`.
