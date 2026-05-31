<!-- markdownlint-disable MD013 -->
# ADR-0260: HIP fourth-consumer kernel — `float_moment_hip` via mirrored kernel-template

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, hip, rocm, amd, kernel-template, fork-local

## Context

This ADR is the sibling to [ADR-0259](0259-hip-third-consumer-ciede.md)
and lands in the same PR. After ADR-0241 (first consumer
`integer_psnr_hip`, int64 SSE) and ADR-0254 (second consumer
`float_psnr_hip`, float partials), the kernel-template's pre-runtime
contract still has one precision posture unproven: **multiple uint64
atomic accumulators in a single kernel pass**.

`integer_moment_cuda.c` (230 LOC, the smallest CUDA twin in the tree
beyond the two PSNR variants already claimed) is the natural choice.
It dispatches once per frame, emits four metrics
(`float_moment_ref{1st,2nd}`, `float_moment_dis{1st,2nd}`) via four
uint64 atomic counters, and the runtime PR will need to memset all
four counters in the same `submit_pre_launch` helper that already
covers `integer_psnr_hip`'s single-counter case. Validating that
shape pre-runtime keeps T7-10b's diff small.

## Decision

### Land `float_moment_hip` as the fourth kernel-template consumer; runtime body still deferred

The PR ships:

- **`core/src/feature/hip/float_moment_hip.{c,h}`** — mirrors
  `core/src/feature/cuda/integer_moment_cuda.c`'s call graph
  verbatim: `init → context_new + lifecycle_init + readback_alloc(4
  uint64) + feature_name_dict`, `submit → submit_pre_launch +
  -ENOSYS`, `collect → collect_wait + -ENOSYS`, `close →
  lifecycle_close + readback_free + dictionary_free +
  context_destroy`. The four-counter readback size
  (`MOMENT_HIP_COUNTERS = 4u`) is named so the runtime PR's host
  reduction has the same constant available verbatim.
- **Registration**: `vmaf_fex_float_moment_hip` is added to
  `feature_extractor_list` in
  `core/src/feature/feature_extractor.c` under `#if HAVE_HIP`,
  immediately after the third consumer. Same scaffold posture as
  ADR-0241 / ADR-0254 / ADR-0259.
- **Smoke test extension**: `core/test/test_hip_smoke.c` grows
  one sub-test (`test_float_moment_hip_extractor_registered`).
- **Meson wiring**: `core/src/hip/meson.build` adds
  `../feature/hip/float_moment_hip.c` to `hip_sources`. No new
  dependency.

### What stays on T7-10b

- Real `hipStreamCreate` / `hipMemcpyAsync` / `hipMallocAsync`
  bodies in `kernel_template.c`; `submit_pre_launch` will memset
  all four counters in one call.
- Per-bpc kernel launch (`calculate_moment_kernel_8bpc` /
  `calculate_moment_kernel_16bpc` HIP twins).
- `VMAF_FEATURE_EXTRACTOR_HIP` flag flip on every consumer.
- Picture buffer-type plumbing for HIP-resident frames.
- Score emission (four
  `vmaf_feature_collector_append_with_dict` calls per frame).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `float_moment_hip` (chosen) | 230 LOC (smallest available CUDA twin), four-uint64-atomic precision posture, one dispatch, validates `submit_pre_launch` memset of multiple counters | Four `provided_features` slightly grow the smoke test's score-emit surface (deferred to T7-10b) | Smallest LOC, distinct precision posture, clean stateful registration. |
| `integer_motion_v2_hip` | Two-feature emission similar to moment | 321 LOC + stateful single-frame-delay reference + dual-feature emission overlap with future T7-10b sibling | Higher complexity, less unique precision posture. |
| `float_ansnr_hip` | 298 LOC, well-defined float-partials posture | Substantively duplicates `float_psnr_hip` (ADR-0254) | Smaller delta vs. ADR-0254. |
| Skip the fourth consumer this PR | Lower review surface | Runtime PR's `submit_pre_launch` would land without a multi-counter pre-runtime gate; multi-counter regression detectable only with a real device | Cheap to land now, expensive to debug post-runtime. |

## Consequences

- **Positive**: kernel-template's "memset multiple uint64 counters
  in one helper call" path is now pinned by a smoke-tested
  consumer. The runtime PR can flip
  `vmaf_hip_kernel_submit_pre_launch`'s body to a single
  `hipMemsetAsync` of `rb.bytes` knowing both the 1-counter
  (psnr_hip) and 4-counter (moment_hip) consumers exercise that
  code path.
- **Positive**: HIP `feature_extractor_list[]` grows by one row
  (fourth row); `vmaf --feature float_moment_hip` returns
  "extractor found, runtime not ready (-ENOSYS at init)" instead of
  "no such extractor".
- **Neutral**: smoke-test sub-test count grows by one (16 with this
  PR + ADR-0259 sibling).
- **Neutral**: T7-10b's surface is unchanged.
- **Negative**: another `-ENOSYS`-pinned file pair to maintain
  until T7-10b lands.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP scaffold-only PR.
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first kernel-template
  consumer (`integer_psnr_hip`).
- [ADR-0254](0254-hip-second-consumer-float-psnr.md) — second
  kernel-template consumer (`float_psnr_hip`); in flight as PR #324.
- [ADR-0259](0259-hip-third-consumer-ciede.md) — third
  kernel-template consumer (`ciede_hip`); sibling of this ADR in
  the same PR.
- [ADR-0221](0221-gpu-kernel-template.md) — original CUDA kernel
  template that ADR-0241 mirrored onto HIP.
- `core/src/feature/cuda/integer_moment_cuda.c` — the CUDA
  reference whose call graph this consumer mirrors.
- `req` — user direction in T7-10b implementation prompt
  (paraphrased: "Land the third and fourth HIP runtime
  kernel-template consumers; pick the cleanest CUDA twins; prefer
  the smallest two by LOC").
