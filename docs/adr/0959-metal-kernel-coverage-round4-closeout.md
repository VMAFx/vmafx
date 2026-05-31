<!-- markdownlint-disable MD060 -->
# ADR-0959: Metal kernel parity coverage round 4 — closeout

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude
- **Tags**: testing, metal, gpu, parity, regression-guard

## Context

The Metal backend ships 8 feature-extractor kernels under
`core/src/feature/metal/` (one `.mm` host wrapper and one `.metal` MSL
source per kernel: `float_moment`, `float_motion`, `float_ms_ssim`,
`float_psnr`, `float_ssim`, `integer_motion`, `integer_motion_v2`,
`integer_psnr`). Per-kernel CPU-vs-Metal parity tests were closed
across three rounds:

| Round | PR | Kernels covered |
|---|---|---|
| 1 (registration) | #351 | 8 extractors discoverable via `vmaf_get_feature_extractor_by_name` |
| 2 (parity, batch A) | #379 | `motion_v2`, `integer_psnr`, `float_psnr`, `float_ssim` |
| 3 (parity, batch B) | #447 | `integer_motion`, `float_motion`, `float_moment`, `float_ms_ssim` |
| **Round 4 (closeout)** | **this PR** | **regression guard + 8/8 closeout audit** |

The gap this ADR closes is *structural*, not per-kernel. Each
round-2 / round-3 test names its target extractor inline
(`"integer_motion_metal"`, etc.). When a future round-5 kernel lands
under `core/src/feature/metal/` without a corresponding `test_metal_*_parity.c`
file, the existing suite passes — there is no test naming the new
kernel, so the gap is silent until someone manually re-runs the
coverage audit. The fork-wide cross-backend parity gate (ADR-0214)
applies *to tests that exist*; it does not enforce that every kernel
has a parity test.

The CUDA round-4 closeout (PR #464, 19/19 kernels) and the SYCL
round-4 closeout (PR #465) set the precedent of shipping a single
audit test that asserts "every kernel in the source tree has a
registered extractor + dispatch row" so the day a kernel ships
without registration the audit goes red. This ADR aligns Metal with
that precedent.

## Decision

We will ship a single `test_metal_kernel_coverage_audit.c` test
that enumerates the 8 expected Metal kernel basenames and asserts:

1. Each `<basename>_metal` resolves to a registered extractor via
   `vmaf_get_feature_extractor_by_name`. CPU-side check — runs on
   every host (Linux / Windows / Intel Mac / Apple Silicon).
2. Each `<basename>_metal` is accepted by
   `vmaf_metal_dispatch_supports`. Needs a `VmafMetalContext`; skips
   cleanly with `-ENODEV` on non-Apple-Family-7 hosts.
3. The dispatch strategy does not falsely report plausible-looking
   non-existent Metal names (`vif_metal`, `adm_metal`, `ciede2000_metal`,
   `ssimulacra2_metal`) as supported — wildcard-regression guard.
4. The basename list size equals the `EXPECTED_KERNEL_COUNT` macro
   (8) — defensive cross-check that catches edits to one half without
   the other.

The audit binds the *grow-together* contract: when a 9th `.mm` kernel
lands, the contributor MUST extend `g_metal_kernel_basenames[]`,
add a registration row, add a dispatch-strategy row, and (per
ADR-0214) ship a per-kernel parity test in the same PR. Otherwise
this audit fails first, before the per-kernel test would have been
needed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Don't add a closeout audit; rely on rounds 1–3 | Zero new code | Silent gap the day a 9th kernel lands; manual re-audit cost compounds | Defeats the point of ADR-0214 cross-backend gate — gate must enforce coverage, not just per-pair parity |
| Generate the audit list from a build-time `glob()` over `core/src/feature/metal/*.mm` | Auto-syncs with the source tree | Meson `files()` doesn't allow globs; build-time codegen adds a CI step; SYCL r4 (PR #465) explicitly rejected this pattern for the same reason | Hand-maintained list with an explicit `EXPECTED_KERNEL_COUNT` is the SYCL / CUDA precedent; trade auto-discovery for one-line audit edit per kernel |
| Add 8 individual `test_metal_<kernel>_registration.c` files | Per-kernel granularity | 8× test binaries for what is one audit; round-1 PR #351 already does per-kernel registration | Already covered by PR #351; round 4 is the *structural* gate, not per-kernel |
| Move the audit into `test_metal_smoke.c` | Single test binary | Smoke is for runtime/lifecycle, not coverage policy; mixing concerns reduces test-failure attribution | Coverage-audit lives in its own binary so a coverage failure is unambiguous in CI logs |

## Consequences

- **Positive**:
  - 100 % Metal kernel coverage is now a structural CI invariant.
  - New Metal kernels MUST register + add a dispatch row + extend the
    audit list in the same PR — the audit fails first otherwise.
  - Aligns Metal with CUDA r4 (PR #464) and SYCL r4 (PR #465)
    closeout precedent — three backends now share the same
    audit-by-enumeration pattern.

- **Negative**:
  - Hand-maintained basename list — one edit per new kernel. Mitigated
    by the failure message which names the missing kernel.
  - Test runs on every CPU build (registration probe is CPU-side); a
    handful of extra `strcmp` calls per `meson test` invocation. Cost
    is sub-millisecond.

- **Neutral / follow-ups**:
  - `core/test/AGENTS.md` — no rebase-sensitive invariant added; the
    audit file is fork-local.
  - `docs/state.md` — closes `T-METAL-KERNEL-PARITY-ROUND4-2026-05-31`
    under "Recently closed".
  - Future round-5 PR (if a 9th Metal kernel lands) bumps the audit
    list and the `EXPECTED_KERNEL_COUNT` macro in the same diff.

## References

- ADR-0214 — cross-backend parity gate (places=4 default).
- ADR-0361 — Metal backend scaffold (T8-1).
- ADR-0421 — first Metal kernel = `motion_v2` (T8-1c).
- ADR-0589 — Metal SSIM-family 1e-3 tolerance bound.
- PR #351 — Metal kernel coverage round 1 (registration audit).
- PR #379 — Metal kernel coverage round 2 (parity for 4 kernels).
- PR #447 — Metal kernel coverage round 3 (parity for remaining 4).
- PR #464 — CUDA kernel coverage round 4 closeout (sibling precedent).
- PR #465 — SYCL kernel coverage round 4 closeout (sibling precedent).
- Source: `req` — "Identify any Metal kernels under
  `core/src/feature/metal/` that still lack a CPU-vs-Metal parity
  test … final sweep to verify 100% kernel coverage or document
  remaining gaps with reason."
