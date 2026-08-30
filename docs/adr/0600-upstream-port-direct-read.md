<!-- markdownlint-disable MD013 MD060 -->
# ADR-0600: Port upstream USE_DIRECT_READ zero-copy input path (Netflix/vmaf@30a6e2a8d)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `upstream-port`, `performance`, `tools`, `cli`, `build`

## Context

Netflix upstream commit `30a6e2a8d` (Kyle Swanson, 2026-05-05) adds a
`USE_DIRECT_READ` compile-time flag to the VMAF CLI tools.  Without it, the
CLI reads each video frame into an intermediate `video_input_ycbcr` buffer and
then copies the data plane-by-plane into a `VmafPicture` via
`copy_picture_data`.  With `USE_DIRECT_READ` enabled the CLI reads frame data
directly into the preallocated `VmafPicture` planes via a new per-format
`fetch_into_vmaf_picture` vtable slot, eliminating one buffer allocation and a
full-frame `memcpy` per frame.

The port touches five files:

- `core/tools/vidinput.h` — new `video_input_fetch_into_vmaf_picture_func`
  typedef, new `fetch_into_vmaf_picture` vtable member, new public declaration.
- `core/tools/vidinput.c` — new `video_input_fetch_into_vmaf_picture()`
  dispatcher.
- `core/tools/yuv_input.c` — `yuv_fetch_into_vmaf_picture()` implementation
  vtable update.
- `core/tools/y4m_input.c` — `y4m_fetch_into_vmaf_picture()` implementation
  (falls back to `-1` when a colour-space conversion is required) + vtable
  update.
- `core/tools/vmaf.c` — `fetch_picture()` restructured so the
  `#ifdef USE_DIRECT_READ` path calls `video_input_fetch_into_vmaf_picture`;
  the non-`USE_DIRECT_READ` path retains the old `video_input_fetch_frame` +
  `copy_picture_data` sequence.  Pool default raised from 2 to 3 pictures.

Two prior agent runs failed at the `y4m_fetch_into_vmaf_picture` step; this
run completed the full port in the existing branch worktree where the previous
agents had already written the changes.

## Decision

Port upstream commit `30a6e2a8d` verbatim (with fork style adjustments:
`(void)fprintf(stderr, …)` for discarded returns, `(int)fread` casts, and
`memcmp(…) != 0` for lint compliance), leaving `USE_DIRECT_READ` as an opt-in
`-D` flag rather than the default.  The old `video_input_fetch_frame` path is
retained under `#else` so all existing tests continue to pass without
rebuilding with the new flag.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Enable `USE_DIRECT_READ` by default in meson | Immediate perf gain for all CLI builds | Changes observable behaviour; needs broader testing before enabling by default | Deferred; can be flipped in a follow-up once CI green |
| Skip the port entirely | No risk | Accumulates divergence from upstream; upstream users benefit from reduced allocations | Not acceptable — sync is a standing project goal |
| Adapt to eliminate the `#ifdef` entirely | Cleaner code | Non-trivial refactor; strays further from upstream parity | Upstream chose the guard; keep parity |

## Consequences

- **Positive**: Fork tracks upstream `30a6e2a8d`; CLI can eliminate one
  allocation + full-frame copy per frame when built with `-DUSE_DIRECT_READ`.
- **Negative**: `y4m_fetch_into_vmaf_picture` returns `-1` for formats that
  require a colour-space conversion (i.e. non-null `convert`), so the direct
  path is limited to native Y4M pixel formats that need no conversion.  This
  matches upstream behaviour.
- **Neutral / follow-ups**: A separate PR or meson option can wire
  `USE_DIRECT_READ` into the default build once smoke-tested.  The `pic_cnt`
  default in the picture pool was raised from 2 to 3 (upstream change) to
  account for the new ordering of alloc-then-read.

## References

- Upstream commit: `30a6e2a8dc5c846ed4e009c8fabb08773c8ccb0e`
  ("libvmaf/tools: add USE_DIRECT_READ, eliminate intermediate buffer and memcpy")
- Related upstream PR: Netflix/vmaf (Kyle Swanson, 2026-05-05)
- Prior upstream ports: ADR-0131, ADR-0132, ADR-0134, ADR-0135, ADR-0142,
  ADR-0143 (port precedents)
- req: "Third attempt at porting upstream commit `30a6e2a8d` (USE_DIRECT_READ zero-copy input path)."
