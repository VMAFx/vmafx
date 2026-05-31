<!-- markdownlint-disable MD013 MD060 -->
# ADR-0196: float_motion GPU kernels — float twin of integer_motion blur+SAD

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, cuda, sycl, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) lists `float_motion` as the
second **Group B** metric — float twin of `integer_motion` (which
ships on CUDA / SYCL / Vulkan). The CPU reference is
[`float_motion.c`](../../core/src/feature/float_motion.c) (276 LOC):

1. `picture_copy(offset=-128)` normalises raw samples to float in
   `[-128, peak-128]`.
2. Separable 5-tap Gaussian blur using `FILTER_5_s`
   (`{0.054488685, 0.244201342, 0.402619947, 0.244201342, 0.054488685}`,
   sums to ~1.0) via `convolution_f32_c_s` (V→H pass).
3. Stored in a 3-frame ring buffer of blurred refs.
4. `motion = sum(|blur[cur] - blur[prev]|) / (w·h)` via
   `compute_motion_simd`.
5. `motion2 = min(motion[i-1], motion[i])` emitted at `index - 1`.

Mirror padding: skip-boundary reflective mirror
(`width - (j_tap - width + 2)` ≡ `2*(width-1) - j_tap`) — same as
`integer_motion.c::edge_8`/`edge_16`, NOT motion_v2's edge-replicating
variant.

## Decision

Ship `float_motion_{vulkan,cuda,sycl}` as **single-dispatch kernels**
(after frame 0) that mirror the integer motion ping-pong but operate
in float:

### Same submit/collect pattern as `motion_*`

- `blur[2]` ping-pong of float-per-pixel blurred ref planes (4 ×
  W·H bytes per slot vs the int kernel's 2 × W·H — same upload
  bandwidth, larger device-side residency).
- One dispatch per frame: load ref pixel → tile → V/H 5-tap blur →
  write to `blur[cur_idx]`. If `frame_index > 0`, also subtract
  `blur[prev_idx]` per pixel and accumulate `|diff|` into per-WG
  float partials.
- Frame 0: emit `motion2 = 0` (and `motion = 0` if debug). Bypass
  SAD via spec/push constant.
- Frame 1+: emit `motion = sum(partials) / (w·h)`. Frame ≥ 2: emit
  `motion2[i-1] = min(prev_motion, motion)`. Final-frame `motion2`
  delivered in `flush()` at `index - 1`.

### Pixel normalization

- bpc=8: `val = (raw - 128.0)`.
- bpc=10/12/16: `val = (raw / scaler) - 128.0` where scaler is
  `4 / 16 / 256`. Matches `picture_copy(offset=-128)`.

The blur preserves the `-128` offset linearly, so it doesn't affect
SAD (constant cancels). We could skip the offset on the GPU side and
get the same SAD score with one fewer FLOP per pixel — but matching
the CPU's float layout exactly keeps the kernel faithful to the
reference, and the offset costs nothing measurable.

### Mirror padding — skip-boundary, matching CPU

GLSL / CUDA / SYCL: `2 * (sup - 1) - idx` for `idx >= sup`. Same as
`motion`'s GPU kernels. **Diverges** from `motion_v2`'s edge-
replicating mirror (called out in
[ADR-0193](0193-motion-v2-vulkan.md) and
[ADR-0194](0194-float-ansnr-gpu.md)) — `float_motion` follows
`integer_motion`'s mirror because that's what its CPU reference
uses.

### Precision contract: places=4 measured

Float convolution + per-WG float SAD reduction + `double` host
accumulation produce small ULP drift vs CPU's strict left-to-right
order. ADR-0192 nominal: `places=3`. Empirical floor on cross-backend
gate fixture (Netflix normal pair, 576×324):

| Backend                                    | Bit depth | Frames | `motion` max_abs_diff | `motion2` max_abs_diff |
|--------------------------------------------|-----------|-------:|----------------------:|-----------------------:|
| Vulkan (Intel Arc A380 + Mesa anv)         |  8-bit    |     48 |         **3.00e-06** |          **3.00e-06** |
| Vulkan                                     | 10-bit    |      3 |         **1.00e-06** |          **0.00e+00** |
| CUDA (NVIDIA RTX 4090)                     |  8-bit    |     48 |         **3.00e-06** |          **3.00e-06** |
| CUDA                                       | 10-bit    |      3 |         **1.00e-06** |          **0.00e+00** |
| SYCL (Intel Arc A380 + oneAPI 2025.3)      |  8-bit    |     48 |         **3.00e-06** |          **3.00e-06** |
| SYCL                                       | 10-bit    |      3 |         **1.00e-06** |          **0.00e+00** |

Identical across backends — same correctness signal as the rest of
batch 3. `places=4` threshold (`5e-5`) cleared by an order of
magnitude.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Reuse motion_v2's linearity trick (single-dispatch over `prev_ref - cur_ref`)** | Skips the blurred-state buffer entirely | Diverges from CPU shape; might introduce small float drift via different reduction order | Match CPU `float_motion` 1:1 for places=4 confidence |
| **3-buffer ring (mirror CPU `blur[3]`)** | Recompute-free `motion2` (CPU does it that way) | Larger device residency; the 2-buffer + `prev_motion_score` cache pattern from `motion_vulkan` already works | 2-buffer + cached prev score is the established pattern |
| **Alias to `motion_*` int kernels** | No new kernels needed | Different input domain (float `[-128, peak-128]` vs uint), different filter semantics | Per ADR-0192: float twins kept native |
| **Skip the `-128` offset on GPU** | One fewer FLOP per pixel | Doesn't affect score (offset cancels in SAD) but trivially diverges from CPU's intermediate values | Faithful to CPU reference; cost is zero |

## Consequences

- **Positive**: closes second of four Group B float twins. Pattern
  reused 1:1 from `motion_*`'s GPU kernels with only the arithmetic
  type changed.
- **Positive**: per-backend kernels at ~200 LOC GLSL + ~250 LOC PTX
  ~340 LOC SYCL. Host glue ~480 LOC each (motion_*'s ping-pong
  shape).
- **Positive**: identical empirical drift across backends — strong
  correctness signal.
- **Neutral / follow-ups**:
  1. CHANGELOG + features.md updates ship in the same PR per
     ADR-0100.
  2. Lavapipe lane gains a `float_motion` step.
  3. Next batch 3 metric: `float_vif` (multi-scale VIF — significantly
     more kernel surface than the previous Group B metrics).

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch 3 scope.
- Sibling: [ADR-0193](0193-motion-v2-vulkan.md) (motion_v2),
  [ADR-0195](0195-float-psnr-gpu.md) (float_psnr).
- Existing motion GPU kernels: motion_vulkan (
  [`motion.comp`](../../core/src/feature/vulkan/shaders/motion.comp) +
  [`motion_vulkan.c`](../../core/src/feature/vulkan/motion_vulkan.c)),
  motion_cuda
  ([`motion_score.cu`](../../core/src/feature/cuda/integer_motion/motion_score.cu) +
  [`integer_motion_cuda.c`](../../core/src/feature/cuda/integer_motion_cuda.c)),
  motion_sycl
  ([`integer_motion_sycl.cpp`](../../core/src/feature/sycl/integer_motion_sycl.cpp)).
- CPU reference:
  [`float_motion.c`](../../core/src/feature/float_motion.c) (276 LOC).
- Verification: cross-backend gate
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  with `--feature float_motion --places 4`. New step in the lavapipe
  lane of
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
