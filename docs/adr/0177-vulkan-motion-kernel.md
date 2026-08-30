# ADR-0177: Vulkan motion kernel + motion cross-backend gate

- **Status**: Accepted (errata 2026-04-26 below — body unchanged per ADR-0028)
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `gpu`, `feature-extractor`, `numerical-correctness`

> **Errata (2026-04-26 same-day)** — the "ULP=0 vs CPU on the
> Netflix normal pair" empirical baseline in the original body
> inherited the silent-CPU-fallback bug from ADR-0176 (see that
> ADR's errata block). The Vulkan motion kernel itself IS clean
> on Arc A380 via Mesa anv (max_abs ≤ 1e-6 vs CPU at places=4).
>
> What the corrected gate ALSO surfaced: the **CUDA and SYCL
> motion kernels both drift by 2.6e-3 against the CPU integer
> reference across 47/48 frames** of the Netflix normal pair —
> identical magnitude on both backends, suggesting shared
> algorithmic inheritance. CUDA's motion uses fused 5×5 Gaussian
> with uint32 accumulation while CPU integer_motion uses separable
> y-then-x with uint16 intermediate; the rounding pattern
> diverges enough to produce a ~0.05% relative drift on the
> final score. Pre-existing — predates all the Vulkan T5-1
> work — but only visible now that the gate works. Tracked as
> a separate kernel investigation (CUDA + SYCL motion drift).

## Context

PR #118 (commit `50758ea8`) landed the Vulkan VIF kernel + cross-backend
gate (ADR-0176). T5-1c extends the Vulkan kernel matrix to ADM, motion,
and motion_v2. This ADR covers the **motion** half of T5-1c (motion +
motion_v2 in one PR, ADM in a follow-up per the user's split decision).

The CPU integer-motion extractor at `core/src/feature/integer_motion.c`
implements a separable 5-tap Gaussian blur (filter
`{3571, 16004, 26386, 16004, 3571}`, sum = 65536) followed by SAD between
the current and previous blurred reference frames. `motion_score = SAD /
256.0 / (W·H)`; `motion2_score = min(prev_motion_score, cur_motion_score)`,
emitted with a one-frame lag.

Two design choices needed an ADR:

1. **motion3 deferred.** The CPU extractor also exposes `integer_motion3`
   in five-frame-window mode (with motion-blend post-processing). That
   path is gated behind a runtime flag and would require a 5-frame ring
   buffer + the blend tables. We defer motion3 to a follow-up.
2. **Cross-backend gate scope.** ADR-0176's `cross_backend_vif_diff.py`
   was VIF-specific. We extend it to a generic per-feature gate by
   adding a `--feature` flag (currently `vif | motion`) so the same
   script + lavapipe + Arc nightly lanes cover every Vulkan kernel as
   they land.

## Decision

We will land a Vulkan compute kernel for `integer_motion` /
`integer_motion2` (no motion3), gated against the CPU scalar reference
by a new lavapipe CI step that reuses the script renamed in spirit
(file path stays `scripts/ci/cross_backend_vif_diff.py` for git-history
continuity) but exposes a `--feature {vif,motion}` selector. The Arc
nightly lane gets the same step.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Bundle motion + motion_v2 + ADM in one PR | One review | 2000+ LOC review; ADM (wavelet) failure blocks motion | Adopted the user's "two PRs" split — motion+motion_v2 first |
| Include motion3 (5-frame window) | Feature parity with CPU | Adds 5-deep ring buffer, motion-blend tables, moving-average post-processing — doubles the scope | Defer; motion3 is opt-in via runtime flag, no production model uses it |
| Separate `cross_backend_motion_diff.py` script | Clear blast radius | Duplicates 90% of the VIF script | Generalized via `--feature` flag; cleaner than cargo-culting two scripts |
| Separate motion CI lane | Independent status check on dashboard | Doubles runner-minutes for the same fixture downloads | Added as a second step in the existing lavapipe + Arc lanes — same fixtures, two diff invocations |

## Consequences

- **Positive**: Vulkan kernel matrix grows to 2/3 of the production CPU
  feature set (VIF, motion). T5-1c reduces to ADM-only. The
  cross-backend script is now reusable for every future kernel via
  `--feature <name>`.
- **Negative**: motion3 is a known gap. Models that depend on motion3
  (none of the shipped ones do) fall back to CPU silently. The two-PR
  split adds a CI cycle and review-context-switch.
- **Neutral / follow-ups**:
  - **T5-1c-motion3** (optional, low priority): port the 5-frame window
    plus motion-blend path. Only needed if a future model requires it.
  - **T5-1c-adm** (next PR): wavelet-based ADM kernel. Significantly
    larger than VIF or motion; expect 3-level wavelet decomposition,
    per-band CSF + masking, and a new shader file per-band or per-scale.
  - The empirical baseline at this commit is **ULP=0** vs CPU on the
    Netflix normal pair (48 frames, both motion + motion2, places=4
    gate clears with zero mismatches).

## References

- ADR-0176 — Vulkan VIF cross-backend gate (the gate this builds on).
- ADR-0175 — Vulkan backend scaffold.
- ADR-0127 — Vulkan backend governance decision.
- PR #118 — T5-1b-v: VMAF_FEATURE_EXTRACTOR_VULKAN + CLI + cross-backend
  gate (commit `50758ea8`).
- `req` — user direction 2026-04-25 (paraphrased): "Two PRs: motion +
  motion_v2 first, ADM second". Selected via popup.
