<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1108: CUDA motion_v2 twin emits motion3_v2_score

- **Status**: Accepted
- **Date**: 2026-06-13
- **Deciders**: Lusoris
- **Tags**: cuda, feature, parity

## Context

The CPU reference extractor `motion_v2` (`core/src/feature/integer_motion_v2.c`)
emits three features in its EOS `flush()`: `motion_v2_sad_score` (per-frame),
`motion2_v2_score` (the min-blend), and `motion3_v2_score` (a per-frame
`motion_blend(motion2, blend_factor, blend_offset)` clipped to `motion_max_val`,
with an optional 2-tap moving average, seeded by a `stamp_value` for indices
below `min_idx = 1`). The CUDA twin `motion_v2_cuda`
(`core/src/feature/cuda/integer_motion_v2_cuda.c`) emitted only the first two —
its `provided_features[]` listed `sad` + `motion2_v2` and its `flush()` stopped
after `motion2_v2`. Its option table carried only `motion_fps_weight`, so the
four `motion3`-driving options (`motion_blend_factor`, `motion_blend_offset`,
`motion_max_val`, `motion_moving_average`) were silently unavailable on the
CUDA path.

[ADR-0337](0337-motion-v2-public-api-options.md) added the full option surface
and `motion3_v2_score` to the CPU extractor, but its Consequences §Neutral
explicitly **deferred** the GPU twins: "GPU twins (CUDA, SYCL, HIP, Vulkan) of
`motion_v2` do not need the option surface in this PR ... whether GPU twins
gain the same options will be decided when each twin needs to emit
`motion3_v2_score`." A `motion_v2_cuda` consumer (a CHUG re-extract, a model
file carrying `motion_v2=motion_blend_factor=…`, or a co-scheduled CPU+CUDA
parity run) that requested `motion3_v2_score` therefore got nothing on the CUDA
path — a genuine feature-coverage gap, not a numerical one. This ADR closes the
deferral for the CUDA twin.

## Decision

We will make `motion_v2_cuda` emit `VMAF_integer_feature_motion3_v2_score` with
the same per-frame formula, ordering, `stamp_value` seeding, and feature-name
dictionary handling as the CPU `integer_motion_v2.c::flush()`. The CUDA twin's
option table gains the four CPU options it consumes — `motion_blend_factor`,
`motion_blend_offset`, `motion_max_val`, `motion_moving_average` — mirroring the
CPU `VmafOption` definitions byte-for-byte (same names, aliases, defaults,
min/max, and `VMAF_OPT_FLAG_FEATURE_PARAM`). The post-process is host-side
scalar over the SAD scores the kernel already produces, so no GPU kernel change
is needed; the `motion_blend` helper is reused verbatim from the shared
`motion_blend_tools.h` header (already consumed by the v1 CUDA twin
`integer_motion_cuda.c`). `motion_force_zero` and `motion_five_frame_window`
stay CPU-only — the CUDA kernel always computes the SAD, and the 5-frame window
remains unsupported per ADR-0337.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Mirror CPU `flush()` host-side over collected SAD scores (chosen) | Bit-exact by construction; no kernel change; reuses shared `motion_blend` | ~40 lines of host loop duplicated across twins | This is the established v1 precedent (`integer_motion_cuda.c`) and the lowest-risk path to bit-parity |
| Replicate `motion_blend` as a static inline in the CUDA TU | Self-contained | Duplicates a shared header that is already includable from CUDA TUs (v1 twin includes it) | Needless duplication; drift risk against the CPU source of truth |
| Promote the CPU streaming post-process (`motion3_postprocess_cuda` style from v1) | Matches v1 twin's streaming shape | v2's `flush()` is a batch loop with `stamp_value` seeding, not a per-frame streaming post-process; reshaping it invites off-by-one bugs | Mirroring v2's own `flush()` is closer to its source of truth |
| Leave the gap, document it | Zero code | Model files referencing `motion3_v2` silently lose the feature on CUDA; breaks co-scheduled CPU+CUDA parity | Defeats the purpose; the deferral was always meant to close "when each twin needs to emit motion3_v2_score" |

## Consequences

- **Positive**:
  - `motion_v2_cuda` now emits `motion3_v2_score`, closing the GPU-twin coverage
    gap that ADR-0337 deferred. Measured CPU-vs-CUDA parity on the Netflix
    `src01_hrc00_576x324` ↔ `src01_hrc01_576x324` pair (48 frames) is
    **max abs per-frame diff = 0.000e+00** at default options and at
    `motion_blend_factor=0.5 + motion_moving_average=1` (places=4, ADR-0214).
  - A model file or CLI invocation carrying `motion_v2_cuda=motion_blend_factor=…`
    now loads and scores identically to the CPU path; the feature-name dict
    suffix (`_mbf_0.5`) matches between CPU and CUDA, so sfr/hfr co-scheduled
    naming stays consistent.
  - `motion2_v2_score` is now emitted via `append_with_dict` (was the bare
    `append`), matching the CPU naming path so renamed co-schedule columns line
    up across backends.

- **Negative**:
  - The host-side `flush()` post-process is duplicated between
    `integer_motion_v2.c` and `integer_motion_v2_cuda.c`. ADR-0141
    (touched-file lint-clean) catches formula drift on the next edit of either
    file; the inline comments cite the CPU source lines.

- **Neutral / follow-ups**:
  - The SYCL, HIP, and Metal twins still emit only `sad` + `motion2_v2` (the
    same gap). This ADR scopes the CUDA twin only; the other twins are tracked
    as follow-ups (`docs/state.md` row T-CUDA-MOTION-V2-MOTION3-MISSING notes
    the sibling gaps). The mirroring is mechanical once the CUDA shape is
    settled.
  - Netflix-golden gate (CPU, places=4, ADR-0024) is unaffected: motion v1 and
    the CPU `motion_v2` are untouched; this change is additive on the CUDA path
    only.

## References

- Supersedes the GPU-twin deferral claim in
  [ADR-0337](0337-motion-v2-public-api-options.md) §Consequences (Neutral /
  follow-ups) for the CUDA twin only. ADR-0337 otherwise remains Accepted.
- Sister ADRs:
  - [ADR-0219](0219-motion3-gpu-coverage.md) — GPU motion3 coverage precedent
    (host-side scalar post-process).
  - [ADR-0358](0358-cuda-motion-race-and-precision-fixes.md) — the v1
    `motion_cuda` flush motion3 emission this twin mirrors.
  - [ADR-0214](0214-gpu-parity-ci-gate.md) — the places=4 GPU-parity tolerance.
- Source: `req` — user direction to make the CUDA `motion_v2` extractor emit
  `motion3_v2_score` bit-exactly matching the CPU reference.
