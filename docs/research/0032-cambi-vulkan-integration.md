# Research Digest 0032 — cambi Vulkan integration trade-offs

- **Date**: 2026-04-29
- **Author**: Claude (Anthropic), reviewed by <lusoris@pm.me>
- **Companion ADR**: [ADR-0210](../adr/0210-cambi-vulkan-integration.md)
- **Predecessor**: [Research-0020](0020-cambi-gpu-strategies.md) — the
  feasibility spike's strategy comparison

## Scope

Research-0020 picked the high-level strategy (II hybrid). This digest
records the *integration-time* trade-offs that surfaced while landing
the kernel and explains why each was resolved the way it was.

## 1. Buffer-pair refactor vs internal-header trampoline

ADR-0205 §"v1 implementation effort estimate" listed
"refactor `calculate_c_values` to take a buffer pair instead of a
`VmafPicture`" at ~50 LOC. In practice the refactor cost is ~200 LOC
because:

- 4 update-histogram-* helpers also take `VmafPicture` and are called
  from 6 sites in `calculate_c_values`.
- 3 SIMD twins (`cambi_avx2.c` / `cambi_avx512.c` / `cambi_neon.c`)
  feed their range-update callbacks through the same struct.
- The CPU extract path (`preprocess_and_extract_cambi`) builds the
  full `VmafPicture` pair anyway from `s->pics[0]` / `s->pics[1]`.

Tactical alternative: keep the CPU signatures unchanged, expose the
file-static helpers via `core/src/feature/cambi_internal.h`, and
let the Vulkan twin allocate its own `VmafPicture` pair as readback
targets. Cost: one full-frame readback per scale.

Per-scale readback at 4K = ~25 MiB (image + mask, 16-bit packed).
PCIe Gen4 ×16 ≈ 32 GiB/s → ~0.8 ms / scale → ~4 ms / frame across
5 scales. The CPU `calculate_c_values` runs 200–250 ms / frame at
4K (per-pixel sliding-histogram update). 4 ms readback overhead is
1.6 % of the host-residual cost — well below the noise floor of
PCIe variance.

**Choice**: trampoline header. Selected.

## 2. Mask DP shader — three TUs vs single TU + spec const

Three per-pass TUs would mean three `.comp` files + three
`spv_embed.h` headers + three pipeline-build sites + three
descriptor-set-layout entries. The kernels share their bindings (1
input, 1 output, both word-packed) and their push constant struct
(width, height, two strides, threshold, pad). The only per-pass
divergence is the `main()` body's branch.

Single TU with `PASS` spec constant:

- Pre-compiles 3 distinct SPIR-V variants at pipeline build via
  `vkCreateComputePipelines`'s `pSpecializationInfo`.
- Runtime cost identical to 3 separate modules (the SPIR-V optimiser
  dead-codes the unused PASS branches).
- Reviewers see a single 130-line file with the SAT semantics
  collocated.

**Choice**: single TU. Selected.

## 3. Per-stage one-shot command buffer vs single per-frame buffer

PSnr-vulkan and motion-vulkan use one command buffer per frame.
ssimulacra2-vulkan uses one per *scale* but inside that records the
full multi-stage pipeline. cambi has 5 scales × ~6 stages (decimate
×2 + filter_mode H + filter_mode V + occasional barrier) plus a
pre-loop spatial-mask pass = ~32 dispatches/frame.

Tactical question: record everything in one command buffer (32
dispatches in one submit) or use 32 one-shot buffers?

Per-stage:

- Simpler debug — each stage's `vkSubmit` returns a discrete error
  if anything fails.
- Higher submit overhead — ~50 µs / submit on lavapipe → ~1.6 ms /
  frame submit overhead.
- Compatible with the per-stage host scratch readback (decimate uses
  scratch_buf as scratch then memcpy back via mapped pointers).

Single buffer:

- Lower submit overhead (~50 µs / frame).
- Forces all per-scale memcpy work onto the GPU (extra
  `vkCmdCopyBuffer` dispatches), or requires staging the memcpys to
  after the buffer-end fence.
- Larger blast radius if a mid-frame dispatch fails — error reporting
  loses scale-granularity.

For v1 the ~1.5 ms submit overhead is dominated by the host residual
(>50 ms even at 576x324). v2 perf polish can collapse to one buffer
once profile data shows submit overhead is the dominant remaining
cost.

**Choice**: per-stage. Selected.

## 4. GPU preprocess shader — wired now or scaffolded?

`cambi_preprocess.comp` exists in-tree (matches the CPU's
`decimate_generic_*_and_convert_to_10b` + optional anti-dither). v1
*does not* dispatch it — the CPU path runs the preprocess on host
and uploads the result.

Reason: the CPU bilinear-resize coordinate arithmetic when source !=
enc resolution uses `(unsigned)(y + 0.5)` integer rounding inside
`for (unsigned i = 0; i < out_h; i++) { float y = start_y + i*ratio_y; }`
The float accumulation across a 4K row at fp32 picks up ~3-5 ULP of
drift relative to the GPU's `gl_GlobalInvocationID.y * ratio_y` per-
thread compute, which can flip the rounded integer at the worst case
~once per 100 K pixels. This violates places=4 silently on
non-exact-resolution inputs.

The GPU shader's exact-resolution fast path (`in_w == out_w &&
in_h == out_h`) is the common case in our test corpus and is
trivially bit-exact. A focused follow-up ADR can wire it on once the
non-exact-resolution path is independently validated.

**Choice**: ship the shader, wire only the host path. Forward-
compatible scaffold. Selected.

## 5. Verification approach

Cross-backend gate at `places=4` (5e-5 absolute tolerance on the
emitted Cambi score). Smoke run of the Netflix normal pair (576x324,
48 frames) confirmed ULP=0 between CPU and Vulkan extractors —
matching ADR-0205's "expected bit-exact" prediction.

The gate row in `scripts/ci/cross_backend_vif_diff.py`'s
`FEATURE_METRICS` dict checks the single `Cambi_feature_cambi_score`
metric. Future gate extensions (e.g., the optional `cambi_source` /
`cambi_full_reference` outputs from `full_ref=true`) would add rows
once the GPU twin grows full-ref support.

## References

- ADR-0205 — feasibility spike + strategy choice.
- ADR-0210 — this PR's integration ADR.
- Research-0020 — strategy comparison.
- `core/src/feature/cambi.c` — CPU reference.
- `core/src/feature/vulkan/cambi_vulkan.c` — Vulkan integration.
