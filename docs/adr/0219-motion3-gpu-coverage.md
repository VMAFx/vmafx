<!-- markdownlint-disable MD013 MD060 -->
# ADR-0219: motion3 GPU coverage on Vulkan + CUDA + SYCL (3-frame window)

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: gpu, vulkan, cuda, sycl, motion, feature-extractor, fork-local, t3-15c, places-4

## Context

The CPU `motion` extractor (`core/src/feature/integer_motion.c`) emits
three outputs per frame: `motion_score`, `motion2_score`, and
`motion3_score`. The GPU twins shipped to date —
`motion_vulkan` (ADR-0177), `motion_cuda`, `motion_sycl` — emitted
only the first two; `motion3_score` was deliberately deferred (see
the `DELIBERATE: motion3_score is omitted` comment block at the end
of `motion_vulkan.c` pre-PR). Backlog item T3-15(c) (formerly
T3-17) tracks closing that gap.

`motion3_score` has two distinct code paths in CPU:

1. **3-frame window** (default, `motion_five_frame_window=false`).
   `motion3 = clip(motion_blend(motion2 * fps_weight, blend_factor,
   blend_offset), max_val)` with optional moving-average against the
   previous unaveraged blended value. **No new device-side state**:
   it's a pure host-side scalar transform of `motion2`, which the
   GPU already produces.
2. **5-frame window** (`motion_five_frame_window=true`). Adds a
   second SAD pair (frame i-2 ↔ i-4), so the device needs a 5-deep
   blur ring buffer. No shipped VMAF model uses this mode; the option
   exists for ad-hoc CLI tuning.

T3-15(c)'s scope is the *coverage* gap, not the 5-frame mode itself.
Closing path 1 across all three GPU backends takes the existing
motion2 SAD output and adds the same scalar post-processing the CPU
runs in `extract()` / `flush()`. Closing path 2 requires re-shaping
the device-side ring buffer in 3 different kernel languages
(GLSL/SPIR-V, CUDA, SYCL), and there is no test that exercises it.

NASA/JPL Power-of-10 rule 6 (declared scope) and CERT C "fail
loud, fail early" both argue for accepting motion3 in the 3-frame
mode and rejecting `motion_five_frame_window=true` with `-ENOTSUP`
at `init()` rather than silently producing wrong answers.

## Decision

We will emit `VMAF_integer_feature_motion3_score` from the
`motion_vulkan`, `motion_cuda`, and `motion_sycl` extractors in
3-frame window mode (the default). The post-processing —
`motion_blend()` + clip-to-`motion_max_val` + optional
moving-average — runs on the host inside the existing `collect()`
/ `extract()` / `flush()` paths, mirroring the CPU extractor's
`integer_motion.c` lines 510-560 byte-for-byte. The full options
surface (`motion_blend_factor`, `motion_blend_offset`,
`motion_fps_weight`, `motion_max_val`, `motion_moving_average`)
is added to the GPU options tables; defaults match the CPU
extractor.

`motion_five_frame_window=true` is rejected with `-ENOTSUP` at
`init()` on all three GPU backends. The cross-backend parity gate
(`scripts/ci/cross_backend_parity_gate.py`,
`scripts/ci/cross_backend_vif_diff.py`) is extended to compare
`integer_motion3` at `places=4` whenever a `--feature motion`
backend pair is exercised.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Extend existing motion kernels (chosen)** | Zero device-code change; pure host scalar add-on; cross-backend numerical parity automatic because motion2 is already gated; tracks CPU algorithm exactly | The 5-frame window mode still deferred (no shipped model needs it) | Best ratio of coverage gain to risk |
| New standalone `motion3_*` extractor per backend | Clean separation of concerns | Triples the kernel-launch path for a metric that is a deterministic post-process of motion2; would need to recompute motion2 internally; doubles the cross-backend gate matrix | Wasted compute, no upside |
| 5-frame ring buffer on device + full motion3 (paths 1 + 2) | Closes both gaps in one PR | 3 kernels (GLSL, CUDA, SYCL) require a 2-deep → 5-deep ring rewrite, second SAD-pair dispatch, new push-constant / spec-constant geometry; no existing fixture tests `motion_five_frame_window=true` | Out-of-budget; no test gate; defer |
| Per-frame readback of blurred buffers + CPU-side 5-frame motion3 | Avoids device-side ring expansion | 4× device-host bandwidth at the only point the GPU pipeline is currently bandwidth-clean; defeats the purpose of GPU offload | Defeats GPU offload |
| Batched readback of N frames | Lower amortised bandwidth | Still requires reshaping the ping-pong protocol; adds latency variance; harder to reason about with the SYCL combined-graph API | More complex than the deferred device path |

## Consequences

- **Positive**:
  - GPU motion now feeds the same downstream model surface as CPU
    motion. Tools that rely on motion3 (e.g. ad-hoc CLI tuning,
    research notebooks at `ai/scripts/phase3_subset_sweep.py`) work
    unchanged across `--backend cuda|sycl|vulkan`.
  - Cross-backend parity gate auto-extends: adding
    `integer_motion3` to `FEATURE_METRICS` exercises every existing
    pair (CPU↔CUDA, CPU↔SYCL, CPU↔Vulkan) at `places=4`.
  - `provided_features[]` for the three GPU twins now matches CPU,
    closing the framework-routing gap that previously sent a
    `--feature integer_motion3` request to the CPU path even with
    `--backend cuda|sycl|vulkan`.

- **Negative**:
  - `motion_five_frame_window=true` is now an *explicit* GPU
    failure (`-ENOTSUP`) where it was previously just unsupported
    by virtue of motion3 absence. Callers that were relying on the
    silent-degradation behaviour will see a hard error.
  - Three additional `double`-typed options enter each GPU
    extractor's option dict, growing the per-extractor state
    struct by ~40 bytes. Negligible at runtime.

- **Neutral / follow-ups**:
  - 5-frame window mode tracked as a sub-task of T3-15(c) /
    `motion_five_frame_window-gpu`. The kernel work is non-trivial
    on all 3 backends; defer until a shipped model needs it.
  - Cross-backend gate at `places=4` should hold trivially since
    motion3 is a deterministic scalar post-process of motion2,
    which already meets `places=4` on all three backends. If a
    future regression surfaces, the root cause is in the motion2
    kernel, not in the new post-processing.
  - The `motion3_postprocess_*` helper triplicates across the three
    extractors. A future refactor could lift it into
    `motion_blend_tools.h` as a shared inline helper alongside
    `motion_blend()`. Not done in this PR — keeping the
    implementation local to each extractor matches the existing
    "each backend owns its score-emission logic" pattern.

## References

- Upstream extractor: `core/src/feature/integer_motion.c` (CPU
  reference; lines 510-560 are the motion3 emission in `extract()`,
  lines 401-438 in `flush()`).
- Sister GPU motion ADRs: [ADR-0177](0177-vulkan-motion-kernel.md)
  (Vulkan motion T5-1c), [ADR-0193](0193-motion-v2-vulkan.md)
  (motion_v2 Vulkan), [ADR-0145](0145-motion-v2-neon.md) (motion_v2
  NEON).
- Cross-backend gate: [ADR-0125](0125-vif-vulkan-bitexact-policy.md),
  [ADR-0138](0138-simd-bit-exactness-policy.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- Backlog: `docs/backlog-audit-2026-04-28.md` row A.1.4 (Vulkan
  motion3) — note that the audit row only mentions Vulkan; this PR
  closes CUDA + SYCL in the same change since the post-processing
  is identical.
- Prior fork close: T4-1 / Netflix#1486 (CPU motion3 already
  present, see [ADR-0158](0158-netflix-1486-motion-updates-verified-present.md)).
- Source: `req` — backlog row T3-15(c): "motion3 (5-frame window)
  GPU coverage on Vulkan + CUDA + SYCL (former T3-17;
  T4-1/Netflix#1486 closed CPU side)."
