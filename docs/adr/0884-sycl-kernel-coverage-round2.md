<!-- markdownlint-disable MD060 -->
# ADR-0884: SYCL kernel coverage round 2 — five additional CPU-vs-SYCL parity gates

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: testing, sycl, gpu, parity, fork-local

## Context

The SYCL backend ships 18 kernels under `core/src/feature/sycl/`. Prior
to round 1 (ADR-0868, PR #351), only three of those had a numerical
cross-backend parity gate: `cambi_sycl` (smoke + score sanity in
`test_integer_cambi_sycl.c`), `motion_sycl` (via `motion3` post-process
in `test_sycl_motion3_parity.c`), and the SYCL state/pic lifecycle
itself (`test_sycl.c`, `test_sycl_pic_preallocation.c`).

Round 1 added `psnr_sycl` + `integer_vif_sycl`. That leaves the high-value
kernels — `adm_sycl` (the dominant feature in every shipping VMAF model),
`ciede_sycl` (Intel-Arc colour-difference path that the Vulkan backend
cannot reach at places=4 per [T-VK-CIEDE-F32-F64](../state.md) /
[ADR-0391](0391-ciede-vulkan-nvidia-f32-f64-precision-gap.md)),
`integer_ssim_sycl`, `float_ms_ssim_sycl` (the 5-scale exponent stack —
most numerically delicate in the SYCL SSIM family), and
`motion_v2_sycl` (the SAD-based motion energy refinement, a separate
kernel topology from motion / motion2 / motion3) — without any
cross-backend assertion in `meson test`.

The risk is silent score drift on Intel-Arc CHUG re-extracts: the
existing cross-backend score gate (`/cross-backend-diff`, ADR-0214) is
run manually, not in CI on every PR. Without per-kernel parity tests
in the `fast` suite, a regression in (for example) the ADM DLM
sub-band convolution would only surface during the next manual
GPU-parity sweep — long after the offending commit had landed.

## Decision

Add five additional `test_sycl_*_parity.c` files mirroring the round 1
pattern (PR #351), each:

- Loads a deterministic synthetic YUV420P fixture (256x144 for
  `adm` / `ciede` / `ssim` / `motion_v2`; 256x192 for `ms_ssim` to
  clear the `11 << 4 = 176` min-dim requirement of the 5-scale
  pyramid).
- Runs the matching CPU scalar feature extractor.
- Initialises `VmafSyclState` with `device_index = -1` (default
  oneAPI device picker) and runs the SYCL feature extractor.
- Asserts the headline score matches within ADR-0214 places=4 (1e-4)
  tolerance.
- Emits `[skip: no SYCL device]` and passes cleanly when
  `vmaf_sycl_state_init()` returns non-zero (no oneAPI runtime,
  no Intel GPU, etc.).

Tests wire into `core/test/meson.build` under the existing
`if get_option('enable_sycl')` guard, suite `['fast', 'gpu']` so they
run in the pre-push gate when a SYCL toolchain is present.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Five per-kernel parity tests (chosen)** | Each test owns a single feature, clear blame on failure; mirrors round 1 / ADR-0868 exactly. | Five new files; some duplication of the fill_pic + feed_frame harness. | Selected — the round 1 pattern is established and reviewers know how to read it. |
| One combined `test_sycl_kernel_parity.c` | Less duplication; one binary to build. | Single failing assertion would obscure which kernel regressed; one-failure-stops-the-run hides cascading issues. | Per-kernel blame is worth the small per-file scaffold cost. |
| Wire kernels into `/cross-backend-diff` only | No new test files; relies on the existing skill. | The skill is an interactive dev-time tool; it doesn't run in CI on every PR. Regressions can land for weeks before the next manual run. | Defeats the purpose of an automated gate. |
| Defer until SYCL CI lane is healthy | Avoids advisory-only test failures during the lavapipe SYCL-CI churn. | Each deferred week is another window where Intel-Arc CHUG runs could silently drift. The skip path means missing-device runs pass cleanly anyway. | Skip-on-no-device is the right cost trade. |

## Consequences

- **Positive**: every high-value SYCL kernel now has a per-PR parity
  assertion against the CPU scalar reference at ADR-0214 places=4.
  Catches single-kernel regressions before they ship.
- **Positive**: closes the round-1 follow-up backlog item — round 1
  acknowledged it covered only psnr + vif and that adm/ssim/ms_ssim
  needed a round 2.
- **Negative**: adds ~50 ms per test to `meson test -C build --suite=fast`
  per kernel when a SYCL device is visible (Intel Arc + dev container).
  Five tests × ~50 ms = ~250 ms additional wall time; negligible
  versus the existing 30-second suite.
- **Neutral / follow-ups**: still missing parity gates for
  `float_psnr_sycl`, `float_vif_sycl`, `float_adm_sycl`,
  `float_motion_sycl`, `float_ssim_sycl`, `integer_psnr_hvs_sycl`,
  `integer_moment_sycl`, `speed_chroma_sycl`, `speed_temporal_sycl`,
  `ssimulacra2_sycl` — round 3 candidates. The float variants share
  most of the kernel topology with their integer twins so they're
  lower-priority than the round-1 + round-2 picks.

## References

- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend places=4 gate.
- [ADR-0219](0219-motion3-gpu-contract.md) — motion3 GPU contract,
  source of the original SYCL parity-test pattern.
- [ADR-0868](0868-gpu-backend-kernel-coverage.md) — round 1 (CUDA/HIP/SYCL
  PSNR + VIF + Metal registration audit).
- PR #351 — round 1 implementation.
- PR #293 — SYCL init-failure cleanup fix (touches `integer_adm_sycl.cpp`,
  `integer_vif_sycl.cpp`, `speed_chroma_sycl.cpp`, `speed_temporal_sycl.cpp`)
  — no test-file overlap with this PR.
- Source: `req` — direct user direction to extend SYCL kernel test
  coverage beyond PR #351 with 5 additional parity tests, avoiding
  PR #293 / PR #351 overlap.
