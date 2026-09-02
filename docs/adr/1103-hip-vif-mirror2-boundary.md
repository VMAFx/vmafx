<!-- markdownlint-disable MD013 MD060 -->
# ADR-1103 — Fix integer_vif_hip boundary condition: clamp_i → mirror2_i

| Field | Value |
|---|---|
| **Status** | Accepted |
| **Date** | 2026-06-13 |
| **Deciders** | lusoris |
| **Tags** | hip, vif, parity, boundary, correctness, fork-local |
| **Supersedes** | ADR-0566 §"clamp matches CPU within places=4" claim (now incorrect) |

## Context

ADR-0563 fixed the catastrophic carry-bit corruption in `integer_vif_hip`
(VMAF ~57 → ~76) by reverting to per-thread `atomicAdd`. After that fix,
a residual parity gap of places~2.75 (max |HIP−CPU| ≈ 0.0018 per VIF scale)
remained on the Netflix src01 576×324 reference pair, violating ADR-0214's
places=4 gate.

The root cause was identified by measuring the actual delta on gfx1030
(RDNA2, wave32) with a current-master build:

| Scale | Max |HIP−CPU| (pre-fix) | Places |
|-------|------------------------|--------|
| scale0 | 0.001788 | ~2.75 |
| scale1 | 0.005541 | ~2.26 |
| scale2 | 0.005375 | ~2.27 |
| scale3 | 0.005368 | ~2.27 |

### Root cause: boundary condition mismatch

The CPU reference (`integer_vif.c`) uses `PADDING_SQ_DATA` before each
horizontal pass — a **symmetric reflect** at the edges:

- index `-1` reads pixel `1` (mirror at 0)
- index `w` reads pixel `w-2` (mirror at `w-1`)

The CUDA twin (`filter1d.cu` lines 121-128) implements the same policy
via a "Two-bounce mirror" in its shared-memory load stage.

The HIP kernel used `clamp_i(idx, 0, dim-1)` — **replicate-edge** boundary.
This disagrees with both CPU and CUDA for the `filter_half_width` pixels at
each edge. For scale 0 (filter width 17, half 8) on a 576-pixel-wide frame,
this affects 16 of 576 columns per row (~2.8%), producing the observed delta.

The log computation (`log_generate` vs CPU LUT `log2_32/log2_64`) was verified
bit-identical for the same normalized 16-bit inputs — it is NOT the source of
the gap.

### Fix: replace clamp_i with mirror2_i in all filter reads

A `mirror2_i(v, dim)` device helper was added, implementing the same
two-bounce mirror as CUDA:

```c
__device__ __forceinline__ static int mirror2_i(int v, int dim)
{
    if (v < 0)
        v = -v;
    if (v >= dim)
        v = 2 * dim - v - 2;
    /* Safety clamp for very small frames. */
    if (v < 0)  v = 0;
    if (v >= dim) v = dim - 1;
    return v;
}
```

All six filter-loop boundary reads in `vif_statistics.hip` were updated:

- Vertical pass (8-bit): `ky = mirror2_i(y - HALF + fi, h)` (× 1)
- Vertical pass (16-bit): `ky = mirror2_i(y - HALF + fi, h)` (× 1)
- Horizontal pass (8-bit): `kx = mirror2_i(x - HALF + fj, w)` and
  `kx = mirror2_i(x - HALF_RD + fj, w)` (× 2)
- Horizontal pass (16-bit template): same two replacements (× 2)

### Device verification (gfx1030, wave32, current master)

Measured on the Netflix src01 576×324 8-bit YUV pair (48 frames), using a
current-master build with `enable_hipcc=true`:

| Scale | Max |HIP−CPU| (post-fix) | Places | Frames perfect |
|-------|-------------------------|--------|----------------|
| scale0 | 0.0000010 | ~6.00 | 46 / 48 |
| scale1 | 0.0000010 | ~6.00 | 43 / 48 |
| scale2 | 0.0000010 | ~6.00 | 39 / 48 |
| scale3 | 0.0000010 | ~6.00 | 32 / 48 |
| **All** | **all 48/48 < 1e-4** | **≥ 4** | |

Pooled VIF scores (48 frames): HIP matches CPU to 7 significant figures.
Pooled VMAF (VIF-only features): HIP=76.667848, CPU=76.667831, delta=0.000017.

## Decision

Replace `clamp_i` with `mirror2_i` in all filter-loop boundary reads in
`core/src/feature/hip/integer_vif/vif_statistics.hip`. Tighten the
in-repo HIP VIF parity test (`test_hip_vif_parity.c`) from `PARITY_TOL=1e-3`
(places=3) to `PARITY_TOL=1e-4` (places=4) per ADR-0214 and ADR-0566.

ADR-0566's claim that "clamp matches CPU within places=4" was incorrect — the
correct statement is that mirror-reflect matches CPU within places=6, and
clamp produces places~2.75.

## Alternatives considered

| Option | Notes | Decision |
|--------|-------|----------|
| Keep clamp_i, accept places=3 | Violates ADR-0214 and ADR-0566. | Rejected |
| LUT-based log (match CPU exactly) | Python simulation showed log computation is bit-identical for the same normalized 16-bit inputs; it is not the source of the delta. | Not applicable |
| Shared-memory tiling (match CUDA exactly) | CUDA uses smem tiles to amortize the mirror boundary; HIP scalar-per-thread avoids the smem complexity. mirror2_i achieves the same numerical result at scalar per-thread cost. | Deferred (performance optimization, not correctness) |
| Accept places=4 VMAF-score gate only (not per-feature) | Per-feature places=4 is the upstream gate per ADR-0566; tightening to per-feature ensures VMAF-score places=4 is achievable via SVM amplification analysis (ADR-0566 §SVM amplification). | Rejected |

## Consequences

- `integer_vif_hip` achieves per-feature places=4 (and in practice places=6)
  on the Netflix golden pair, satisfying ADR-0214 and ADR-0566.
- The in-repo test `test_hip_vif_parity.c` now asserts places=4, catching
  any future regression to clamp-style boundary.
- `clamp_i` is removed from the file (no remaining callers). The `mirror2_i`
  helper remains inline, incurring no additional register pressure vs clamp.

## References

- ADR-0214 — Cross-backend parity gate (places=4 at VMAF-score level)
- ADR-0537 — integer_vif_hip crash fixes
- ADR-0552 — Superseded wavefront XOR-reduce (carry-bit bug)
- ADR-0563 — Per-thread atomicAdd fix (carry-bit fix for integer_vif_hip)
- ADR-0566 — HIP VIF per-feature places=4 gate (this ADR confirms it is achievable)
- CUDA twin: `core/src/feature/cuda/integer_vif/filter1d.cu` lines 121-128
- CPU reference: `core/src/feature/integer_vif.h` `PADDING_SQ_DATA` lines 95-118
- Verified: gfx1030 (RDNA2 wave32), ROCm 7.2.53211, 48 Netflix src01 frames
