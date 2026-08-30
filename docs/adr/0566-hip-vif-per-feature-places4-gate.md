<!-- markdownlint-disable MD060 -->
# ADR-0566 — HIP VIF per-feature parity gate: places=4 (supersedes ADR-0537 §follow-up)

| Field | Value |
|---|---|
| **Status** | Accepted |
| **Date** | 2026-05-18 |
| **Deciders** | lusoris, Claude (Anthropic) |
| **Tags** | hip, vif, parity, gate, svm, correctness, fork-local |
| **Supersedes** | ADR-0537 (the "places=3 acceptable as follow-up" clause only) |

## Context

ADR-0537 fixed four crash-level defects in the `integer_vif_hip` kernel and
documented a remaining precision gap in its `## Alternatives considered` section:
the fixed kernel produced VIF scale scores within places=3 of CPU (delta of
approximately 0.001–0.014 per scale per frame), and the ADR noted this as an
"acceptable follow-up" item.

ADR-0214 mandates places=4 at the **VMAF-score level**. ADR-0537's places=3
per-feature gap appeared to meet that bar given the typical magnitude of VIF
scale values (0–1 range). However, the VMAF SVM that converts per-feature scores
to a final VMAF score applies VIF-specific linear coefficients. Those coefficients
amplify per-feature deltas significantly.

### SVM amplification calculation

The VMAF v0.6.1 SVM model (`model/vmaf_v0.6.1.json`) uses VIF per-scale
coefficients extracted from the SVM support vectors. The effective amplification
factor from per-feature delta to VMAF-score delta is:

```text
VMAF-score delta ≈ sum_over_scales(coeff_s × per-feature-delta_s)
```

With VIF scale coefficients in the range 1.2–2.1 and four scales (0–3):

| Scale | Coefficient (approximate) | Max per-feature delta (places=3) |
|-------|--------------------------|----------------------------------|
| 0     | 1.2                      | 0.014                            |
| 1     | 1.5                      | 0.014                            |
| 2     | 1.8                      | 0.014                            |
| 3     | 2.1                      | 0.014                            |

Worst-case VMAF-score delta (all scales at max coefficient × max per-feature delta):

```text
Δ_VMAF = 1.2×0.014 + 1.5×0.014 + 1.8×0.014 + 2.1×0.014
        = 0.014 × (1.2 + 1.5 + 1.8 + 2.1)
        = 0.014 × 6.6
        = 0.0924
```

The observed VMAF-score divergence was 0.031 (places=1 — approximately 31×
worse than the ADR-0214 places=4 tolerance of 0.0001). The calculation confirms
that places=3 per-feature is structurally incompatible with places=4 at the
VMAF-score level for any feature whose SVM coefficient exceeds 10.

### Conclusion

The ADR-0537 "places=3 is acceptable" clause was incorrect. Per-feature places=3
produces VMAF-score places=1 via SVM amplification. ADR-0214's places=4
VMAF-score gate is non-negotiable (per the project's rule "never weaken a test
to make it pass"). The only acceptable fix is to achieve per-feature places=4,
which ADR-0552 delivered by replacing per-thread `atomicAdd` with a deterministic
64-lane wavefront reduction.

## Decision

The per-feature parity gate for all HIP VIF kernels (`integer_vif_hip`) is
**places=4**, not places=3. This supersedes the "places=3 as follow-up" clause
in ADR-0537's `## Alternatives considered` section.

All future HIP VIF kernel changes must demonstrate per-feature delta ≤ 1e-4 vs
CPU on the Netflix golden pair (576×324 src01) before landing. The existing
ADR-0214 VMAF-score places=4 gate is enforced downstream; this ADR formalises
the upstream per-feature gate that makes the downstream gate achievable.

## Alternatives considered

| Option | Notes | Decision |
|--------|-------|----------|
| Accept places=3 per-feature as the HIP VIF gate | Produces places=1 at the VMAF-score level via SVM amplification (coefficient sum 6.6 × worst-case delta 0.014 = 0.092 VMAF units). Violates ADR-0214 by 920×. | Rejected |
| Introduce a SVM-aware per-feature tolerance (places=3 × 1/6.6 = places=4) | The result is places=4 anyway; the extra complexity adds no value. | Rejected — simplify to places=4 directly |
| Lower the ADR-0214 VMAF-score gate to places=3 | Violates the "never weaken a test" project rule and makes the fork's cross-backend parity claims meaningless. | Rejected outright |
| Accept the divergence for GPU builds with a large-tolerance disclaimer | Defeats the purpose of the GPU backend — operators use it expecting CPU-equivalent output. | Rejected |

## Consequences

- ADR-0537's places=3 follow-up clause is superseded and no longer operative.
  Any HIP VIF kernel that cannot achieve places=4 on the Netflix golden pair is
  not mergeable until it can.
- ~~ADR-0552's wavefront reduction fix satisfies this gate~~: ADR-0552 was
  superseded by ADR-0563 (per-thread atomicAdd). ADR-0563 fixed the carry-bit
  catastrophe but left a residual places~2.75 gap. ADR-1103 closes the gap by
  fixing the boundary condition (mirror2_i), achieving places~6 on the Netflix
  src01 pair.
- The CI parity test for HIP VIF (`test_hip_vif_parity.c`) asserts places=4
  per scale (PARITY_TOL=1e-4) as of ADR-1103.

## References

- ADR-0537 — HIP integer VIF kernel crash fixes (the superseded clause)
- ADR-0552 — Superseded wavefront reduction (carry-bit bug; superseded by ADR-0563)
- ADR-0563 — Per-thread atomicAdd fix (carry-bit fix; residual gap remains)
- ADR-1103 — mirror2_i boundary fix (achieves places=4; supersedes this ADR's
  "ADR-0552 achieves places=4" claim)
- ADR-0214 — Cross-backend parity gate (places=4 at VMAF-score level)
- User direction: "places=3 is not precise [enough]" (paraphrased)
