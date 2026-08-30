<!-- markdownlint-disable MD060 -->
# ADR-0193: motion_v2 Vulkan kernel — single-dispatch SAD via convolution linearity

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, gpu, feature-extractor, fork-local, bit-exact

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) scopes batch 3 with
`motion_v2` as the first metric: smallest CPU reference (300 LOC),
builds directly on the already-shipped
[`motion_vulkan`](../../core/src/feature/vulkan/motion_vulkan.c)
([ADR-0177](0177-vulkan-motion-kernel.md)) — same 5-tap separable
Gaussian filter, same int64 partial-sum reduction, same
VkSpecializationInfo shape.

The CPU reference is
[`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c).
Stateless variant of `integer_motion`: instead of storing blurred
ref frames between `extract` calls, it exploits convolution
linearity:

```text
SAD(blur(prev), blur(cur)) == sum(|blur(prev - cur)|)
```

so each frame computes its score in one V→H separable convolve
over `(prev_ref - cur_ref)`, accumulating `|h|` directly into the
SAD instead of writing blurred frames to a side buffer.

## Decision

Ship `motion_v2_vulkan` as a **single-dispatch GLSL compute
kernel** with **GPU-side raw-pixel ping-pong**:

### Single dispatch per frame (after frame 0)

The kernel consumes both prev and cur ref pixels via two
read-only SSBOs and emits per-WG `int64` SAD partials. Frame 0 is
short-circuited host-side without a dispatch (CPU emits 0.0 too).
No blurred output buffer — the diff is filtered and reduced inline.

### GPU-side raw-pixel ping-pong

Two `ref_buf[2]` SSBOs hold the most recent two ref planes. Each
frame uploads only the current ref into `ref_buf[cur_ref_idx]`,
then binds `ref_buf[1 - cur_ref_idx]` as "prev". Halves the per-
frame upload bandwidth vs the alternative of using the framework's
`fex->prev_ref` (which would require uploading both this frame's
ref AND the previous frame's ref every call).

### Mirror padding — diverges from `motion.comp`

> **Correction (ADR-0662, 2026-05-20):** the literal below was stale.
> Current CPU `integer_motion_v2.c::mirror` uses `2 * size - idx - 2`,
> and the CUDA / SYCL / Vulkan twins must match that reflect-101 formula.
> The old `-1` value produced the lavapipe drift fixed by ADR-0662.

CPU `integer_motion_v2.c::mirror` as originally documented here:

```c
if (idx >= size) return 2 * size - idx - 1;   /* edge replication */
```

CPU `integer_motion.c::edge_8` / `edge_16`:

```c
if (i_tap >= height) i_tap = 2 * (height - 1) - i_tap;  /* no edge replication */
```

These differ by **one** at the right/bottom boundary. `motion.comp`
implements the second formula (matches its CPU twin); this kernel
must implement the first one. Catching this offset cost the only
debug round in bring-up — initial implementation reused
`motion.comp`'s `dev_mirror` verbatim and produced
`max_abs_diff = 2.62e-3`. The fix is one line in `dev_mirror` and
gets `max_abs_diff` to `0.0`.

### Precision contract: bit-exact (`places=4` in the gate)

CPU computes the full pipeline in integer arithmetic (`int32` /
`int64` accumulators with explicit round-and-shift). The shader
matches the same arithmetic shape:

- Vertical accum: `int64`, then `(acc + (1 << (bpc-1))) >> bpc` →
  `int32` cell in `s_vert`.
- Horizontal accum: `int64`, then `(h + 32768) >> 16` → `int64`.
- `abs(blurred)` → per-thread `int64` contribution.
- `subgroupAdd` → per-WG slot in the `int64` SAD SSBO.
- Host-side: `int64` sum across slots, `/ 256.0 / (W·H)` → final
  `motion_v2_sad_score`.

`int64` is pinned by `GL_EXT_shader_explicit_arithmetic_types_int64`.
The vertical accumulator could fit `int32` for `bpc <= 12` but
overflows at `bpc=16` (filter[k]=26386 × diff range ±65535 × 5 taps
≈ 8.6e9 > INT32_MAX), so the kernel uses `int64` throughout for
uniformity — the perf cost on Arc / Mesa is negligible (the kernel
is bandwidth-bound on the input load).

Empirical verification (Intel Arc A380 + Mesa anv driver, Netflix
normal pair 576×324):

| Bit depth | Frames | `motion_v2_sad_score` max_abs_diff | `motion2_v2_score` max_abs_diff |
|-----------|-------:|-----------------------------------:|--------------------------------:|
| 8-bit     |     48 |                       **0.0e+00** |                    **0.0e+00** |
| 10-bit    |      3 |                       **0.0e+00** |                    **0.0e+00** |

`places=4` gate threshold (`5e-5`) is several orders of magnitude
above the actual delta — the contract could be raised to
`places=17` if a use case ever demands it. Setting it at `places=4`
keeps it consistent with the rest of the GPU long-tail.

### `motion2_v2_score` emitted host-side in `flush()`

The `motion2_v2_score = min(score[i], score[i+1])` post-process
runs on collected feature scores after the per-frame pipeline
finishes — same shape as CPU `integer_motion_v2.c::flush`. No GPU
work needed; the kernel only emits `motion_v2_sad_score`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **2-dispatch (V then H, intermediate buffer)** | Cleaner halo handling — each pass is embarrassingly parallel within a row | Needs an `int32` intermediate buffer of size `W·H` (≈730 KB at 576×324, ≈8.3 MB at 1080p). Twice the dispatch cost. | The single-dispatch tile-with-halo design from `motion.comp` is already reviewed and optimal at the WG sizes we use; reusing it 1:1 (modulo the diff load and dropped output) is the smallest possible delta on top of motion. |
| **Use framework's `fex->prev_ref` and re-upload prev each frame** | Matches CPU code shape exactly; no GPU-side ping-pong state | 2× upload bandwidth per frame (prev + cur) vs ping-pong's 1× (cur only). Negligible at 576×324, noticeable at 4K. | Ping-pong is the same pattern `motion_vulkan` already uses for blurred frames — symmetric scaffolding, lower bandwidth. |
| **Spec-constant for `bpc` instead of push constant** | Compiler can specialise the `>> bpc` shift; one less push-const per dispatch | Already a spec constant via `BPC` (constant_id = 2). Push constants only carry runtime params (width/height/wg_count). | This is what we do — flagged for the audit trail. |
| **Drop `int64` for `bpc <= 12`** | `int32` accumulators free up a register lane; small perf win on integrated GPUs | Two pipeline variants to maintain (low-bpc vs high-bpc); kernel is bandwidth-bound anyway, no measurable speedup. | YAGNI; the uniform `int64` path costs nothing measurable and keeps the shader simple. |

## Consequences

- **Positive**: bit-exact match to CPU on both 8-bit and 10-bit
  paths. Strongest possible precision contract — the gate runs at
  `places=4` for parity with the rest of the long-tail but the
  actual floor is exact.
- **Positive**: ~280 LOC kernel + ~280 LOC host glue. Smallest
  fork-local kernel PR in batch 3, validates the "delta on top of
  motion" thesis from ADR-0192's per-metric ordering.
- **Negative**: the original mirror divergence warning became stale
  after the CPU reference settled on the reflect-101 `2 * size - idx - 2`
  formula. ADR-0662 corrects the Vulkan / CUDA / SYCL twins and keeps
  lavapipe parity gated.
- **Neutral**: motion3 is irrelevant for motion_v2 (no 5-frame
  window mode in the v2 algorithm); no equivalent of motion.comp's
  scope exclusion needed.

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch 3 scope.
- Sibling kernel: [ADR-0177](0177-vulkan-motion-kernel.md) — the
  motion Vulkan kernel this one builds on.
- CPU reference:
  [`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c)
  (300 LOC).
- Mirror divergence vs `motion.comp`:
  [`motion_v2.comp`](../../core/src/feature/vulkan/shaders/motion_v2.comp)
  `dev_mirror` comment block.
- Verification: cross-backend gate
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  with `--feature motion_v2 --places 4`. New step in the lavapipe
  lane of
  [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
