<!-- markdownlint-disable MD013 MD060 -->
# ADR-0267: HIP sixth kernel-template consumer — `motion_v2_hip`

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (T7-10b follow-up)
- **Tags**: `hip`, `gpu`, `feature-extractor`, `kernel-template`, `temporal`

## Context

The first five HIP kernel-template consumers ([ADR-0241](0241-hip-first-consumer-psnr.md),
[ADR-0254](0254-hip-second-consumer-float-psnr.md), [ADR-0259](0259-hip-third-consumer-ciede.md),
[ADR-0260](0260-hip-fourth-consumer-float-moment.md), [ADR-0266](0266-hip-fifth-consumer-float-ansnr.md))
are all **non-temporal**: each frame is independent of its neighbours.
The runtime PR (T7-10b) needs at least one temporal consumer in place
before it lands so the helper-body flip can validate the
`flush()`-callback path and the cross-frame buffer-carry shape that
motion-class metrics require.

This ADR adds the **sixth consumer** — `motion_v2_hip`, the HIP twin
of `core/src/feature/cuda/integer_motion_v2_cuda.c` (320 LOC).
`motion_v2` is the simplest temporal extractor in the CUDA tree:
single-dispatch (uses convolution linearity to compute SAD over
`prev - cur` directly), single int64 atomic accumulator, plus a
device-side ping-pong of raw ref Y planes so the next frame can read
the current frame's plane as "prev".

## Decision

We add `core/src/feature/hip/integer_motion_v2_hip.{c,h}` as the
sixth kernel-template consumer. The TU mirrors the CUDA twin
call-graph-for-call-graph, including the `flush()` callback and the
`VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag bit. The ping-pong buffer pair
is tracked as `uintptr_t` slots in the state struct (no actual
allocations performed in the scaffold body) — the runtime PR (T7-10b)
will land a HIP device-buffer allocator and replace these with real
handles. `init()` returns `-ENOSYS` until T7-10b; the smoke test
pins the registration shape; CHANGELOG and rebase-notes carry the
addition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `motion_v2_hip` (chosen) | Smallest temporal CUDA twin (320 LOC) — pins the temporal-extractor shape (`flush()` callback, ping-pong buffer carry) that no prior consumer exercises. Single dispatch per frame; convolution-linearity trick keeps the kernel small. | Introduces a `uintptr_t[2]` ping-pong slot field that the runtime PR will need to swap for a real device-buffer handle once a HIP buffer-alloc helper exists. | — |
| `float_motion_hip` (361 LOC) | Older motion variant; single dispatch. | 13% larger than `motion_v2_cuda.c`; carries blurred-frame ping-pong instead of raw-pixel ping-pong (more complex state). | Defer to a later batch. |
| `integer_motion_hip` (518 LOC) | Pins the multi-dispatch motion shape. | 62% larger than `motion_v2_cuda.c`; multi-dispatch makes it a worse fit for "smallest unported CUDA twin". | Defer to a later batch. |
| Skip a temporal consumer in this PR | Keeps the diff smaller. | Leaves the runtime PR (T7-10b) without a temporal-shape consumer to validate `flush()` and ping-pong-carry paths against. | Adds a follow-up PR for no real saving. |

## Consequences

- **Positive**: The temporal-extractor shape is now pinned ahead of
  T7-10b. The `flush()` callback's degenerate "no-frames-collected"
  case is exercised, so the runtime PR can swap in the real
  `min(score[i], score[i+1])` post-pass without restructuring the
  `flush()` signature. The `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag
  bit on a HIP extractor is now exercised in the registry.
- **Negative**: Introduces a `uintptr_t[2]` ping-pong slot field
  that has no analogue in the kernel-template — the field shape
  is a load-bearing artefact the runtime PR will re-target. The
  AGENTS.md note pins this so a refactor doesn't drift it.
- **Neutral / follow-ups**: When the runtime PR lands a HIP
  buffer-alloc helper, this TU's ping-pong slots become the first
  consumer of it. The CUDA twin's `VmafCudaBuffer *pix[2]` field
  shape is the target the runtime PR will mirror.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP audit-first scaffold (T7-10).
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first consumer + kernel template.
- [ADR-0254](0254-hip-second-consumer-float-psnr.md) — second consumer.
- [ADR-0259](0259-hip-third-consumer-ciede.md) — third consumer.
- [ADR-0260](0260-hip-fourth-consumer-float-moment.md) — fourth consumer.
- [ADR-0266](0266-hip-fifth-consumer-float-ansnr.md) — fifth consumer (this PR).
- [ADR-0246](0246-gpu-kernel-template.md) — GPU kernel-template pattern.
- CUDA twin: `core/src/feature/cuda/integer_motion_v2_cuda.c`.
- Source: `req` (user dispatch) — "Add the fifth and sixth HIP runtime kernel-template consumers. ... Pick two more cleanest CUDA twins to port."
