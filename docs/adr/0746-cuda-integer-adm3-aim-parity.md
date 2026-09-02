<!-- markdownlint-disable MD060 -->
# ADR-0746: integer_adm_cuda — emit integer_adm3 + integer_aim (parity with CPU)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `cuda`, `integer_adm`, `aim`, `adm3`, `parity`

## Context

`integer_adm_cuda.c` is the default CUDA ADM extractor (integer path is faster
than the float path).  PR #75 cross-backend baseline audit found that it did NOT
emit `VMAF_integer_feature_aim_score` or `VMAF_integer_feature_adm3_score`,
while the CPU `integer_adm.c` extractor does.  Consumers of `pooled_metrics`
who request these features on `--backend cuda` received NaN / missing values.

ADR-0574 added `aim_score` + `adm3_score` to `float_adm_cuda` in 2026-05-18,
but the integer path (the CUDA default) was not updated.

The integer ADM pipeline is distinct from the float path:

- DWT bands are stored as `int16_t` (scale 0) / `int32_t` (scales 1-3).
- CSF and CM are fully inlined into fused kernels; there are no separate
  `decouple_r`, `decouple_a`, `csf_a`, `csf_f` device buffers.
- Accumulation uses fixed-point `int64_t` with cube-root reduction.

## Decision

Extend `integer_adm_cuda.c` and `integer_adm/adm_cm.cu` to compute and
emit `VMAF_integer_feature_aim_score` and `VMAF_integer_feature_adm3_score`,
ULP-equal to the CPU `integer_adm.c` path at `places=4`.

**Implementation strategy: fully-inlined AIM CM kernels — no new device
buffers.**

The AIM CM pass in the CPU code swaps the roles of `decouple_a` (signal) and
`decouple_r` (threshold).  Because the CUDA integer kernels already inline both
decouple computations, the AIM pass is expressed by writing two new kernel
entry points:

- `i4_adm_cm_aim_line_kernel_fused` (scales 1-3, int32 path): same structure
  as `i4_adm_cm_line_kernel_fused` but signal = `a_val` (inline `csf_a`)
  and threshold neighbourhood = inline `1/30 * |rfactor * r_val|` for each
  of the 9 neighbourhood pixels.
- `adm_cm_aim_line_kernel_8` (scale 0, int16 path): same structure as
  `adm_cm_line_kernel` but threshold uses inline `r_val`-based filter values
  and signal = inline `csf_a` (rfactor * a_val).

Both kernels set `noise_weight = 0`, matching the CPU
`i4_adm_cm(..., noise_weight=0.0, measure_aim=true)` call.

The `RES_BUFFER_SIZE` constant is extended from 24 to 36 (adding 12 AIM CM
accumulator slots) so the existing D2H copy and host-side `conclude_adm_cm`
can be reused for the AIM accumulators.

**Host-side post-processing (write_scores):**

```text
score_aim  = aim_num / den   (same denominator as ADM2)
score_adm3 = MAX(score * adm_dlm_weight + (1 - score_aim) * (1 - adm_dlm_weight),
                 adm_min_val)
```

Matches the CPU `integer_adm.c::extract()` formula exactly.  Two new options
are exposed: `adm_skip_aim` (default `false`) and `adm_dlm_weight` (default
`0.5`), matching the CPU defaults.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reuse float kernel from integer path | No new kernel code | Defeats integer-path perf advantage; mixes fp32 and int64 accumulators; precision characteristics differ | Rejected — defeats the entire point of the integer path |
| Separate AIM CSF buffer (like float_adm_cuda ADR-0574) | Symmetric with float_adm approach | Requires new buffer alloc (same size as csf_f), one extra kernel launch, extra VRAM | Not needed — full inline costs ≤ 9× decouple_r recomputes per neighbourhood; decouple is light math |
| Post-process AIM from existing adm_cm accumulators | Zero new kernel code | Mathematically impossible — the AIM threshold swap changes which pixels are masked; the accumulator values are not equivalent | Not viable |
| Separate adm_aim_csf.cu file | Clean separation | No benefit at this scale; kernels share inlines from adm_decouple_inline.cuh | Not justified |

## Consequences

- `VMAF_integer_feature_aim_score` and `VMAF_integer_feature_adm3_score` are
  now emitted by `integer_adm_cuda` on every frame, matching the CPU path.
- `adm_skip_aim=true` disables both and skips the AIM kernel launches.
- Per-frame launch count increases by up to 4 (2 new kernel entries × 1 per
  frame for scale-0 + 1 per scale for scales 1-3 = 4 additional launches for
  the non-skip case).
- `RES_BUFFER_SIZE` 24 → 36: D2H copy grows by 96 bytes per frame (negligible).
- No new device buffers.
- SYCL / Vulkan / HIP `integer_adm` twins do not yet emit `aim_score` /
  `adm3_score`; this ADR covers CUDA only.

## References

- req: user task 2026-05-28: "make integer_adm_cuda emit both features, ULP-equal
  to integer_adm_cpu's emission (NOT float_adm_cuda's)"
- ADR-0574 — float_adm_cuda AIM/ADM3 port (Phase 1)
- ADR-0214 — GPU parity CI gate (places=4 tolerance)
- `core/src/feature/integer_adm.c` — CPU reference implementation
- `core/src/feature/cuda/AGENTS.md` — twin-update rules, parity invariant
