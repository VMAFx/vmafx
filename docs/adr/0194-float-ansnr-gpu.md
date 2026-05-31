<!-- markdownlint-disable MD013 MD060 -->
# ADR-0194: float_ansnr GPU kernels — single-dispatch 3x3 + 5x5 filters with per-WG float partials

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, cuda, sycl, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) scopes batch 3 with
`float_ansnr` as the second metric: tiny CPU reference (124 LOC),
no integer twin upstream, second-most-impactful gap on the matrix
after `motion_v2`.

The CPU reference is
[`float_ansnr.c`](../../core/src/feature/float_ansnr.c) +
[`ansnr.c`](../../core/src/feature/ansnr.c) +
[`ansnr_tools.c`](../../core/src/feature/ansnr_tools.c). The
2D-filter path (the active build's default — `ANSNR_OPT_FILTER_1D`
and `ANSNR_OPT_NORMALIZE` are both off) is structurally simple:

1. `picture_copy` normalises raw samples to float in `[-128, 127.something]`
   (`(raw / scaler) - 128.0`; scaler is `1`/`4`/`16`/`256` for
   bpc=8/10/12/16).
2. **3x3 ref filter** (`{1,2,1; 2,4,2; 1,2,1}/16`) → `ref_filtr`.
3. **5x5 dis filter** (Netflix-tuned weights summing to 1.0) → `filtd`.
4. **MSE-style reductions**:
   `sig += ref_filtr²`, `noise += (ref_filtr − filtd)²`.
5. **Final transforms** on the host:
   `float_ansnr  = 10·log10(sig / noise)` (or `psnr_max` if
   `noise == 0`),
   `float_anpsnr = MIN(10·log10(peak² · w · h / max(noise, 1e-10)), psnr_max)`.

Per-bpc constants `peak` / `psnr_max` come from the CPU `init`
table.

## Decision

Ship `float_ansnr_vulkan`, `float_ansnr_cuda`, and
`float_ansnr_sycl` as **single-dispatch kernels** that produce
**per-WG (sig, noise) float partials** reduced to a single
`double` pair on the host:

### Single dispatch per frame

The kernel reads ref + dis raw pixels, applies both filters
inline from a shared/SLM tile of size 20×20 (16-thread WG plus 2-
pixel halo on every edge — dictated by the 5x5 dis filter), and
emits two floats per workgroup: `sig` and `noise`.

### Float partials → host double accumulation

Per-pixel contributions stay in float (max value ≈ 16384 at
bpc=8). Per-WG sums are in float (≤256 pixels × 16384 ≈ 4.2M,
fits float precision). Across all WGs the host accumulates in
`double` to retain places=4 — same shape as ciede ([ADR-0187](0187-ciede-vulkan.md)).

Float atomicAdd is **not** required; per-WG partials sidestep
both the precision and the atomics-availability questions.

### Picture upload — raw-pixel staging

For all three backends:

- **Vulkan**: host packs the (potentially strided) Y plane into a
  tightly-packed staging buffer, flushes, dispatches, waits.
- **CUDA**: device-to-device `cuMemcpy2DAsync` from the
  `VmafPicture` plane (already on device) into a tightly-packed
  staging buffer, then kernel launch on the picture's stream.
- **SYCL**: pinned-host staging + `q.memcpy` to device USM, then
  kernel launch on the SYCL queue. Self-contained submit/collect
  (does NOT register with `vmaf_sycl_graph_register` because
  ansnr's graph integration adds no value at this PR's complexity
  level).

### Mirror padding — diverges from `motion.comp` (again)

CPU `ansnr_tools.c::ansnr_filter1d_s` / `ansnr_filter2d_s` use
the **edge-replicating** reflective mirror
(`2 * size - idx - 1`), same as
[`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c)
and the float_ansnr GPU twins. NOT the skip-boundary mirror
(`2 * (size - 1) - idx`) used by `motion`'s GPU kernels. Same
footgun called out in [ADR-0193](0193-motion-v2-vulkan.md); the
ansnr kernels inherit the warning.

### Precision contract: places=4 measured (places=3 nominal per ADR-0192)

Float convolution + float per-WG reduction + `log10` final
transform: ULPs vs the CPU's strict left-to-right accumulator
order are inevitable. ADR-0192's nominal contract is `places=3`;
empirical floor on the cross-backend gate fixture (Netflix normal
pair, 576×324):

| Backend                                    | Bit depth | Frames | `float_ansnr` max_abs_diff | `float_anpsnr` max_abs_diff |
|--------------------------------------------|-----------|-------:|---------------------------:|----------------------------:|
| Vulkan (Intel Arc A380 + Mesa anv)         |  8-bit    |     48 |              **6.00e-06** |               **4.00e-06** |
| Vulkan                                     | 10-bit    |      3 |              **2.00e-06** |               **3.00e-06** |
| CUDA (NVIDIA RTX 4090)                     |  8-bit    |     48 |              **6.00e-06** |               **4.00e-06** |
| CUDA                                       | 10-bit    |      3 |              **2.00e-06** |               **3.00e-06** |
| SYCL (Intel Arc A380 + oneAPI 2025.3)      |  8-bit    |     48 |              **6.00e-06** |               **4.00e-06** |
| SYCL                                       | 10-bit    |      3 |              **2.00e-06** |               **3.00e-06** |

`places=4` threshold is `5e-5` — the actual floor is roughly an
order of magnitude tighter. The cross-backend gate runs at
`places=4` for parity with the rest of the long-tail; the
contract could be tightened to `places=4` officially without
risk.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Two dispatches (V then H separable filter)** | Cleaner halo handling | The CPU reference uses 2D `ansnr_filter2d_s` (not the separable variant `ansnr_filter1d_s`). Splitting into V→H would deviate from the CPU shape and add an intermediate buffer for no win | Single 2D pass with a 20×20 shared tile is the smallest deviation from CPU |
| **Atomic float adds to a single sig + noise pair** | One global write, no per-WG slots | Float atomicAdd is supported on Vulkan only with `GL_EXT_shader_atomic_float` (extension); on SYCL via `atomic_ref<float>` with relaxed memory order. Precision on cross-WG associativity drift is also worse than per-WG float + host double | Per-WG float partials match the ciede / motion_v2 pattern and produce identical results across all three backends |
| **Pre-convert ref / dis to float on the host before upload** | Kernel reads float directly, simpler code | 2× upload bandwidth; the conversion is trivial (read uint, multiply, subtract 128 — three FLOPS per pixel) | Convert in shader; keeps ABI clean |
| **Use ANSNR_OPT_FILTER_1D path (separable 1D filter)** | Cheaper compute (3+5 taps vs 9+25 taps) | The active CPU build has it disabled (the 2D path is the reference). Switching the GPU kernel would diverge from CPU output by the difference between the 1D and 2D filters (which are not identical — `ansnr_filter1d_dis` is a different set of weights from the 2D dis filter) | Match the CPU it's porting |

## Consequences

- **Positive**: places=4 numerical agreement on all three backends
  on both 8-bit and 10-bit, with **identical** max_abs_diff
  numbers across backends — strong evidence the kernel logic is
  correct (any algebraic bug would produce backend-specific
  drift). Smallest GPU-twin batch yet (<200 LOC kernel each, ~250
  LOC host glue).
- **Positive**: closes one more matrix gap. After this batch,
  ANSNR moves from "CPU-only float, no GPU twin" to "all three
  GPU backends at places=4".
- **Negative**: float-only metric — no integer twin to compare
  against, so the cross-backend gate is the only precision
  arbiter. Fine at the current empirical floor, but if a future
  driver / kernel change drifts the gate, ADR-0192's `places=3`
  fallback applies.
- **Neutral / follow-ups**:
  1. CHANGELOG + features.md updates ship in the same PR per
     ADR-0100.
  2. Lavapipe lane gains a `float_ansnr` step alongside the existing
     `motion_v2` etc. steps.
  3. Next batch 3 metric: float twins of int kernels already on
     GPU (`float_psnr` / `float_motion` / `float_vif` / `float_adm`).

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch 3 scope.
- Sibling: [ADR-0193](0193-motion-v2-vulkan.md) — first batch 3 metric.
- CPU reference:
  [`float_ansnr.c`](../../core/src/feature/float_ansnr.c) +
  [`ansnr.c`](../../core/src/feature/ansnr.c) +
  [`ansnr_tools.c`](../../core/src/feature/ansnr_tools.c).
- Verification: cross-backend gate
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  with `--feature float_ansnr --places 4`. New step in the
  lavapipe lane of
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
