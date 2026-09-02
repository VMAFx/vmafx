<!-- markdownlint-disable MD013 -->
# ADR-0181: Global feature-characteristics registry + per-backend dispatch strategy

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, cuda, sycl, vulkan, architecture, fork-local

## Context

Today's GPU dispatch decisions are scattered, per-backend, and
keyed on the wrong axis:

- **SYCL** has a single global graph-replay heuristic in
  [`core/src/sycl/common.cpp`](../../core/src/sycl/common.cpp)
  with `GRAPH_AREA_THRESHOLD = 1280×720`: enable graph replay
  when frame area ≥ 720p, direct submit otherwise. Per-state
  (whole context), not per-feature.
- **CUDA** has no conditional dispatch — always uses streams
  directly. The graph-capture API is available but unused.
- **Vulkan** has no conditional dispatch — every kernel records
  a fresh primary command buffer per frame. Secondary cmd-buf
  reuse is available but unused.

The right axis is **per-feature, not per-frame-size and not
per-card**. Per-card / per-generation tuning is intractable
(would require benching every NVIDIA gen, every Intel Xe tile,
every AMD CDNA, plus future HIP / Metal hardware). Per-feature is
the natural cardinality: motion has 2 dispatches per frame and
benefits least from batching; VIF has 4 scales and benefits
middle; ADM has 16 (scale × stage) dispatches per frame and
benefits most.

About to add ~14 new GPU kernels (psnr, ssim, ms_ssim,
ssimulacra2, ciede, cambi, psnr_hvs, ansnr, moment, motion_v2,

- float variants) × 3 backends = up to 42 new dispatch sites.
Without consolidation, each new kernel writes its own
`if (frame_size > X) graph_replay = on` decision per backend.
T7-26 wants to fix that *before* the new kernels land.

## Decision

We introduce a **global feature-characteristics registry**
consumed by **thin per-backend dispatch-strategy modules**.

**Registry** —
[`core/src/feature/feature_characteristics.{c,h}`](../../core/src/feature/feature_characteristics.h).
One descriptor per feature, hung off `VmafFeatureExtractor`:

```c
typedef struct VmafFeatureCharacteristics {
    /// Number of distinct kernel dispatches per frame for this
    /// feature. Drives the per-frame fixed-overhead amortisation
    /// calculation. e.g., VIF=4 scales, ADM=16 (scale × stage),
    /// motion=2 (blur + SAD reduction), psnr=1 (single SE
    /// reduction).
    unsigned n_dispatches_per_frame;

    /// Pure reduction (no per-pixel kernel work besides the
    /// reduction). Reduction-only kernels benefit least from
    /// graph replay because the per-frame work scales linearly
    /// with pixel count and dominates the fixed setup cost.
    bool is_reduction_only;

    /// Minimum frame area (w * h pixels) above which graph-
    /// replay / batching wins versus direct submit. Below this,
    /// fixed per-frame setup overhead dominates the kernel work.
    /// 0 = no preference; backend picks a sensible default.
    unsigned min_useful_frame_area;

    /// Backend-agnostic hint about which dispatch primitive maps
    /// best for this feature. Backends translate to their own
    /// primitives (CUDA graph capture / SYCL graph replay /
    /// Vulkan secondary cmd-buf reuse).
    VmafFeatureDispatchHint dispatch_hint;
} VmafFeatureCharacteristics;
```

`VmafFeatureExtractor` gains a `VmafFeatureCharacteristics chars`
field. Existing extractors seed it as part of this PR (12 rows
total: vif, motion, adm + their float variants, ssim, ms_ssim,
ssimulacra2, cambi, psnr, psnr_hvs, ciede, moment, ansnr, lpips,
motion_v2). Defaults to all-zero (= "no preference") for
extractors not yet seeded; backends fall back to current global
behaviour.

**Per-backend glue** — three thin modules, each ~150 LOC:

- [`core/src/sycl/dispatch_strategy.{c,h}`](../../core/src/sycl/dispatch_strategy.h) —
  consumes `VmafFeatureCharacteristics` + frame dims + env
  overrides, returns `VmafSyclDispatchStrategy { DIRECT,
  GRAPH_REPLAY }`. Migrates the existing
  `GRAPH_AREA_THRESHOLD` logic from
  `core/src/sycl/common.cpp`. Env override:
  `VMAF_SYCL_DISPATCH=<feature>:graph,<feature>:direct,...` (per-
  feature overrides) — supersedes the existing
  `VMAF_SYCL_USE_GRAPH` / `VMAF_SYCL_NO_GRAPH` knobs (kept as
  aliases for one release cycle, then deprecated).

- [`core/src/cuda/dispatch_strategy.{c,h}`](../../core/src/cuda/dispatch_strategy.h) —
  returns `VmafCudaDispatchStrategy { DIRECT, GRAPH_CAPTURE }`.
  Default behaviour today is always DIRECT; registry-driven
  decisions enable opt-in graph capture for high-dispatch-count
  features (ADM mainly) when the frame is large enough.

- [`core/src/vulkan/dispatch_strategy.{c,h}`](../../core/src/vulkan/dispatch_strategy.h) —
  returns `VmafVulkanDispatchStrategy { PRIMARY_CMDBUF,
  SECONDARY_CMDBUF_REUSE }`. Default today is always
  `PRIMARY_CMDBUF`; opt-in reuse is the future optimisation
  surface for ADM (16 dispatches/frame is the obvious
  candidate).

**MVP scope** (this PR): registry + glue modules + descriptor
rows for every existing extractor + migrate SYCL's existing
`GRAPH_AREA_THRESHOLD` logic to the registry. **No new GPU
kernels.** No CUDA graph capture or Vulkan cmd-buf reuse yet —
the strategies expose the primitives but every existing
extractor's descriptor lands with `dispatch_hint = AUTO` (= use
backend default). The point of MVP is to verify the architecture
under existing load before adding 14 new metrics.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Per-backend in-place duplicate logic for every new feature | No new abstraction; each kernel is self-contained | 42 future dispatch sites with copy-pasted heuristics; refactor cost compounds with every new metric | T7-26 exists specifically to prevent this |
| Per-card / per-generation tuning tables | Reflects empirical reality on every supported hardware | Intractable: 4+ NVIDIA gens × 3 Intel Xe tiles × 4 AMD families × 2 Apple gens = 32+ permutations to maintain; tuning drifts with driver updates | Cannot scale; per-feature axis captures 80% of the win with one table |
| Runtime auto-tuning (mini-bench at startup, pick strategy) | No hard-coded heuristics; adapts to hardware | Adds startup latency (50-200ms); requires a representative reference frame; outputs vary across runs (non-determinism in CI snapshots); needs cache invalidation when hardware/driver changes | Defer — useful follow-up if the static descriptors prove insufficient. Static registry covers MVP. |
| Hang descriptors on `VmafFeatureExtractor` (chosen) | Single source of truth; reads naturally next to the extractor's other metadata; no global table to keep in sync | Header change ripples through every extractor TU | Worth it — alternative is a separate central table that drifts out of sync with the extractor list |

## Consequences

- **Positive**: future GPU kernels add one descriptor row + zero
  backend-side dispatch logic. Tuning the heuristic is one place
  to edit. SYCL `GRAPH_AREA_THRESHOLD` becomes a per-feature
  decision instead of a per-context decision (so ADM can
  graph-replay even at small frames where motion shouldn't).
  Foundation for T7-26 follow-ups (CUDA graph capture for ADM,
  Vulkan secondary cmd-buf reuse for ADM, future HIP / Metal).
  Closes T7-17 (SYCL fp64-emulation slowdown — separate, but the
  registry can express the per-feature fp64 dependency too) and
  T7-18 (Vulkan-on-NVIDIA dispatch overhead) as concrete
  instances once the strategies land.
- **Negative**: `VmafFeatureExtractor` header gains a field —
  every extractor TU recompiles. One-time build hit, no ABI
  break (extractors are statically linked into libvmaf;
  registration is in-tree). Env override surface adds
  documentation work in
  [`docs/development/`](../development/) and
  [`docs/backends/`](../backends/) — same-PR per CLAUDE.md
  rule 10.
- **Neutral / follow-ups**:
  1. Once the registry lands, migrate **CUDA** to opt-in graph
     capture for ADM (separate PR; descriptors already in place).
  2. Migrate **Vulkan** to secondary cmd-buf reuse for ADM
     (separate PR; descriptors already in place).
  3. Per-feature fp64-aspect descriptors for SYCL T7-17 (a
     descriptor field `requires_fp64: bool` lets the SYCL
     dispatch_strategy refuse-or-emulate on Arc-A380 with one
     line of code).
  4. Auto-tuning startup mini-bench as the optional layer
     (deferred — see Alternatives).

## References

- Source: user direction 2026-04-26 ("global, not per-backend
  ... just a module for every backendtype and done").
- Backlog row: T7-26 in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md).
- Related: [ADR-0127](0127-vulkan-backend-decision.md) (Vulkan
  backend decision); [ADR-0175](0175-vulkan-backend-scaffold.md)
  (Vulkan scaffold).
- Existing dispatch logic this consolidates:
  [`core/src/sycl/common.cpp:855-866`](../../core/src/sycl/common.cpp)
  (current `GRAPH_AREA_THRESHOLD` decision).
- Subsumes / closes: T7-17 (SYCL fp64-less device performance —
  becomes a per-feature strategy), T7-18 (Vulkan-on-NVIDIA
  dispatch overhead — becomes a per-feature batching decision).
