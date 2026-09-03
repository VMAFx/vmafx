<!-- markdownlint-disable MD060 -->
# ADR-0539: integer ADM HIP kernels — real implementation replacing weak HSACO stubs

- **Status**: Superseded by [ADR-1167](1167-adm-cm-row-level-rounding.md)
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: hip, gpu, feature, integer-adm, port

## Context

ADR-0533 (PR #1292) wired the integer ADM HIP extractor into the dispatch
table, but the four `.hip` kernel sources it depends on
(`adm_dwt2.hip`, `adm_csf.hip`, `adm_csf_den.hip`, `adm_cm.hip`) did not
build standalone via `hipcc --genco`:

- `adm_dwt2.hip` and `adm_csf.hip` were near-line-for-line CUDA ports
  that already compiled, but had never been registered in
  `hip_kernel_sources`.
- `adm_csf_den.hip` had an incomplete port — the first kernel was
  missing the `template <int val_per_thread, int cta_size>` declaration
  and the `__device__ __forceinline__ void` qualifiers, so the file
  failed to parse.
- `adm_cm.hip` was wrapped in an unterminated `#if defined(COMPARE_FUSED_SPLIT)`
  block and referenced CUDA-only helpers (`uint64_cu`, `warp_reduce`,
  `VMAF_CUDA_THREADS_PER_WARP`, `atomicAdd_int64`, `__float2uint_ru`,
  `__log2f`) that `cuda_helper.cuh` provides but `hipcc` does not.

ADR-0536 (PR #1296) papered over the missing strong symbols with weak
fallbacks in `hip_hsaco_stubs.c` so the host link succeeded; at runtime
`hipModuleLoadData` on an empty blob returned non-zero and the extractor
silently fell back to CPU. The user directive — "no stubs anywhere" —
requires real, working kernels.

## Decision

Port the four kernels to compile standalone under `hipcc --genco`, register
them in `hip_kernel_sources`, and delete the four ADM weak slots from
`hip_hsaco_stubs.c`. The xxd-embedded `_hsaco` strong symbols from the
new targets supply the previously-stubbed blobs.

The CUDA twin's per-warp `__shfl_down_sync` reduction (`cuda_helper.cuh::warp_reduce`)
is replaced by **per-thread `atomicAdd` on the 64-bit unsigned accumulator**.
This mirrors the pattern adopted in `vif_statistics.hip` (ADR-0537): AMD
wavefronts are 64 wide (not the 32 the CUDA shuffle mask hard-codes), and
`__shfl_down_sync` is not portable across hipcc / NV. Per-thread atomicAdd
on a 64-bit unsigned accumulator is **bit-exact** with respect to the CUDA
twin — only the reduction *order* differs, and unsigned integer addition is
associative and commutative.

The two-kernel pattern for `adm_cm` (`i4_adm_cm_line_kernel` writing per-thread
scratch + `adm_cm_reduce_line_kernel_4` consuming it) is preserved — the HIP
host TU (`integer_adm_hip.c`) still launches both, mirroring the pre-fusion
CUDA shape. The newer fused CUDA kernel (`i4_adm_cm_line_kernel_fused`)
is not adopted here; migrating the HIP host TU is a follow-up.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Per-thread atomicAdd on uint64 accum (chosen)** | Bit-exact w.r.t. CUDA twin (uint64 add is associative). Trivial to read. Same approach as `vif_statistics.hip` (ADR-0537). Works on every AMD wavefront width without `#ifdef`. | Higher atomic traffic than per-warp reduce. Negligible at 576x324; revisit if profiling shows contention on 4K / HDR ROI. | Selected. |
| HIP `__shfl_down` with `warpSize` (runtime) | Closer to CUDA twin shape. | `__shfl_down` semantics differ between hipcc 5.x and 6.x; `warpSize` is a compile-time constant on AMD but 32 vs. 64 differ between gfx generations; would need per-arch dispatch. | Complexity / portability cost not justified for a four-kernel port. |
| `__hip_atomic_compare_exchange` CAS-loop reduction | Avoids 64-bit atomicAdd on older GCN. | atomicAdd-on-uint64 is native on gfx90a / gfx10 / gfx11 (every dev-MCP target); CAS-loop is the implicit fallback for older silicon already. | Manual fallback redundant. |
| Compile kernels via host-side `hipcc` with `cuda_helper.cuh` shim | Reuses CUDA twin verbatim. | `cuda_helper.cuh` pulls `ffnvcodec/dynlink_loader.h` and assumes warpSize=32 in the shuffle mask; HIP support would need a separate shim header maintained in parallel — same surface area as just porting. | Net negative — more files to maintain, no clarity benefit. |

## Consequences

- **Positive**: HIP backend now produces **bit-exact** ADM scores vs.
  CPU on the Netflix golden src01 pair (verified diff = 0.000000 across
  all six emitted features: `integer_adm`, `integer_adm2`, `integer_adm3`,
  `integer_adm_scale[0-3]`). Closes the silent CPU-fallback bug
  ADR-0533 introduced.
- **Positive**: `hip_hsaco_stubs.c` drops from four stub slots to
  zero; the weak-fallback macro is retained as a pattern for future
  in-progress ports but is currently used by zero extractors.
- **Negative**: Per-thread atomicAdd is theoretically slower than the
  CUDA per-warp reduce. Not measured here; defer profiling to a perf
  pass once ADM HIP is exercised on 4K HDR fixtures.
- **Neutral / follow-ups**: Host TU (`integer_adm_hip.c`) untouched —
  same kernel names, same launch shapes, same `WarpShift` struct layout.
  The HIP host TU still uses the two-kernel reduce pattern; CUDA's
  newer fused variant could be ported as a follow-up if perf becomes
  a concern.

## References

- ADR-0533 (PR #1292): integer ADM HIP extractor wiring.
- ADR-0536 (PR #1296): weak HSACO stubs that this ADR removes for the four ADM blobs.
- ADR-0537: `vif_statistics.hip` adopted the same per-thread atomicAdd
  reduction pattern that this ADR carries into ADM.
- ADR-0214: cross-backend numerical-parity gate (places=4).
- Upstream CUDA twins: `core/src/feature/cuda/integer_adm/adm_*.cu`.
- Source: `req` (user direction): "no stubs anywhere".
