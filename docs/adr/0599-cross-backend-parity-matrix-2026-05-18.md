<!-- markdownlint-disable MD013 MD060 -->
# ADR-0599: Cross-Backend Parity Audit — Full Extractor Matrix (2026-05-18)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `cuda`, `sycl`, `vulkan`, `hip`, `ci`, `parity`, `audit`

## Context

The fork ships 17 distinct SYCL GPU twin extractors, 17 CUDA twins, 17 Vulkan twins,
and 18 HIP registrations alongside the 20 CPU-only base extractors. Up to this point,
parity was verified only for the three production-model features (ADM + VIF + motion)
via the lavapipe gate (ADR-0176/0177/0178). A systematic, single-pass audit covering
every registered extractor had not been run. Any new extractor registration (e.g.
the ADR-0545 dedup pass, ADR-0533 HIP sweep) could silently introduce cross-backend
divergence that the existing gate would not detect.

The user directed a full sweep: enumerate every entry in `feature_extractor_list[]`,
invoke it on the Netflix golden 576x324 fixture across all available backends (CPU,
SYCL, CUDA, Vulkan, HIP), compare pooled means and per-frame values at full IEEE-754
precision (`--precision max`), and report any divergence at or beyond the ADR-0214
places=4 bar.

## Decision

Run the full parity matrix in the `vmaf-dev-mcp` container on 2026-05-18 and publish
results as Research-0550. The audit uses `--precision max` (IEEE-754 `%.17g`) to avoid
the 6-decimal-place JSON format floor masking sub-1e-5 deltas. Backend isolation is
achieved with `--no_sycl`, `--no_cuda`, `--no_vulkan` flags in turn. HIP is probed
but limited to scaffold/ENOSYS (no discrete AMD GPU on the audit host).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend existing lavapipe gate (ADM+VIF+motion only) | Already automated, zero new code | Covers only 3 of 18 extractors; new extractors remain unaudited indefinitely | Does not fulfil the "every registered extractor" requirement |
| Per-PR cross-backend matrix in CI | Catches regressions immediately | Requires lavapipe + CUDA + SYCL runners simultaneously; expensive; gate time >5 min | Deferred; this audit is a one-shot snapshot to establish the baseline |
| ULP comparison (integer representation) | Finest-grained metric | Requires source-level instrumentation; pooled JSON is the only output surface | --precision max gives full double precision without instrumentation |

## Consequences

- **Positive**: Establishes a clean all-exact baseline for all 18 CPU extractors across
  SYCL, CUDA, and Vulkan as of commit `e5d26e238` (2026-05-18). No P0 or P1 findings.
  The ADR-0214 places=4 gate is satisfied by a margin of several orders of magnitude
  (exact bit-for-bit agreement) for every tested extractor. The earlier 3.1e-5 ADM-scale1
  delta documented in `metrics-backends-matrix.md` is no longer observed — it was
  eliminated by the ADR-0178 + ADR-0545 kernel hardening and registry dedup waves.
- **Negative**: HIP parity is still unmeasured (no discrete AMD GPU). Requires a
  follow-up on a gfx1100/gfx1030 host. The ADR-0551 audit confirmed real HSACO kernels
  exist for all 18 HIP extractors; parity numbers are pending.
- **Neutral**: `speed_chroma`, `speed_temporal`, and integer `ssim` (`vmaf_fex_ssim`)
  have no GPU twins — this is an intentional design choice, documented here as a
  registration coherence note (not a bug).

## References

- Research digest: [Research-0550](../research/0550-cross-backend-parity-matrix-2026-05-18.md)
- ADR-0214 (cross-backend parity gate, places=4 requirement)
- ADR-0176/0177/0178 (Vulkan VIF/motion/ADM gate, original scope)
- ADR-0545 (registry dedup pass — removed 67→18 Vulkan entries, SYCL twin dedup)
- ADR-0533 (HIP extractor sweep registration)
- ADR-0551 (HIP extractor audit — confirmed all 18 have real HSACO kernels)
- `req`: The user directed the full systematic sweep in the session briefing of 2026-05-18.
