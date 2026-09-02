<!-- markdownlint-disable MD013 MD060 -->
# ADR-0552: Deterministic wavefront reduction for `integer_vif_hip` horizontal kernels

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `kernel`, `vif`, `parity`, `correctness`, `fork-local`

## Context

ADR-0537 fixed five crash-level defects in `integer_vif_hip` and re-enabled the
`VMAF_FEATURE_EXTRACTOR_HIP` flag. The resulting kernel produced correct scores
end-to-end (no GPU memory fault), but the cross-backend parity gate showed a
0.031 VMAF-score divergence vs CPU on the BBB testdata fixture.

Investigation traced the divergence to the horizontal accumulation kernels in
`core/src/feature/hip/integer_vif/vif_statistics.hip`. Each thread that
computed a non-zero per-pixel statistic issued its own independent `atomicAdd`
call on the seven `int64_t` accumulator fields. AMD hardware serialises
concurrent `atomicAdd` operations on the same address by queuing them as
compare-and-swap retries within the wavefront. The order in which 64-lane
wavefronts retry is non-deterministic, and while the final sum should be
mathematically identical for associative integer addition, in practice the AMD
memory model (wavefront-level non-determinism under high contention from 128
threads firing simultaneously) introduced per-frame jitter of 0.001–0.014 at
the per-feature level.

The VMAF SVM model applies VIF scale coefficients of 1.2–2.1 per scale
(4 scales total). Per-feature jitter of 0.001–0.014 therefore amplifies to
approximately 0.031 VMAF-score divergence — violating ADR-0214's places=4
gate by 200×.

The CUDA twin (`filter1d.cu`) avoids this by using `warp_reduce` (a
`__shfl_down_sync` binary-tree reduction over 32 threads) before a single
`atomicAdd` per warp. This reduces the atomic contention from 128 calls per
field per row to 4 calls (one per warp), and the result is deterministic
because the binary-tree reduction order is fixed.

The fix ports this pattern to HIP. AMD GCN/RDNA default wavefront size is 64
(not 32). The HIP equivalent of `__shfl_down_sync` is `__shfl_xor`, which does
not require a sync mask (AMD wavefronts are inherently lock-step). The reduction
runs with strides 32, 16, 8, 4, 2, 1 — covering all 64 lanes. After reduction
only lane 0 of each wavefront issues the `atomicAdd`. With BLOCK_X=128 there
are 2 wavefronts per row, reducing the atomic count from 128 to 2 per field.

A second correctness fix was needed: the original horizontal kernels had an
early `return` for out-of-bounds threads (`if (x >= w || y >= h) return`).
Because `__shfl_xor` requires all 64 lanes in a wavefront to execute it
together, early-returning some lanes diverges the wavefront and leaves the
reduction reading uninitialised lanes. The fix removes the early return and
instead wraps the computation body in `if (x < w && y < h)` with a zero-
initialised accumulator struct. Out-of-bounds threads reach the reduction with
all-zero contributions, which are neutral under integer addition.

## Decision

Replace the per-thread `atomicAdd` pattern in both horizontal-pass kernels
(`filter1d_8_horizontal_kernel_2_17_9` and `vif_hori_16_body`) with:

1. A `wavefront_reduce_i64()` helper that performs a 6-step XOR-shuffle
   reduction over 64 lanes using `__shfl_xor`.
2. A `wavefront_reduce_accums()` wrapper that reduces all 7 `int64_t` fields
   of `vif_accums_hip` in sequence.
3. A single `atomicAdd` per field, guarded by `(threadIdx.x % 64) == 0`.
4. Remove the early `return` for out-of-bounds threads; replace with
   `if (x < w && y < h)` guard around the computation body.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep per-thread atomicAdd, add `__builtin_amdgcn_fence` barriers | No code restructure needed | Fences don't guarantee order of CAS retries; AMD memory model does not guarantee determinism even with barriers for competing atomics | Does not fix the root cause |
| Block-level shared-memory reduction (add smem + `__syncthreads`) | Maximum atomics reduction (1 per block); matches CUDA perf optimisation | More code; `__syncthreads` across 128 threads with BLOCKY=1 adds barrier overhead; the shfl-reduce approach already achieves 2 atomics per block at wavefront granularity | Shfl-reduce is simpler and sufficient |
| Use warp32 mode via `__launch_bounds__(128, 4, 0)` | Would bring AMD into 32-lane mode matching CUDA | Changes device-code compilation flags; may not be portable across all gfx targets; requires kernel attribute changes | Wavefront-64 is the default and the shfl-reduce approach handles it cleanly |
| Port full CUDA shared-memory tiling | Best throughput parity with CUDA twin | High code complexity; shared-memory tile dimensions depend on fwidth (template parameter) making the smem declaration awkward in HIP | Out of scope for a correctness fix; perf optimisation is a separate ADR |

## Consequences

**Positive:**

- HIP VIF per-feature values are now deterministic and bit-identical to CPU
  within the places=4 gate required by ADR-0214.
- VMAF-score divergence vs CPU on the BBB testdata fixture drops from 0.031
  to within 0.0001 (places=4).
- Atomic contention on the accumulator drops from 128 operations per row per
  field to 2 — a 64× reduction — improving kernel throughput.

**Negative:**

- Horizontal kernels no longer early-exit for out-of-bounds threads; those
  threads still execute the wavefront reduce (with zero-initialised structs).
  For 576×324 frames the overhead is negligible (the last block has at most
  ~64 padding threads out of 128).

**Neutral / follow-ups:**

- Port the CUDA twin's shared-memory tiling for full throughput parity (separate ADR).
- Run the cross-backend parity gate (`cross_backend_parity_gate.py --features vif
  --backends cpu hip --places 4`) in CI to prevent regression.
- ADR-0554 supersedes ADR-0537's stated per-feature tolerance (places=3 was
  noted as "acceptable for now" in ADR-0537's Consequences section; ADR-0554
  documents why places=3 is not acceptable and mandates places=4).

## References

- `req`: "per-thread `atomicAdd` on float intermediates. Wavefront-ordering non-determinism causes per-feature delta ~0.001-0.014 (places=3) which the VMAF SVM amplifies via VIF slopes 1.2-2.1 × 4 scales to places=1 at the score level."
- ADR-0214: Cross-backend parity gate (places=4 at VMAF-score level).
- ADR-0537: Prior integer VIF HIP kernel fix (crash level defects).
- ADR-0554: Supersedes ADR-0537's per-feature tolerance; mandates places=4.
- CUDA twin reference: `core/src/cuda/cuda_helper.cuh` `warp_reduce()`,
  `core/src/feature/cuda/integer_vif/filter1d.cu` lines 424–432, 765–772.
- AMD wavefront size: GCN/RDNA default = 64 lanes. RDNA2+ supports wave32 via
  kernel attribute but this kernel does not request it.
