<!-- markdownlint-disable MD013 MD060 -->
# ADR-0482: Expand vmaf_pre FFmpeg filter device strings to match full VmafDnnDevice enum

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris
- **Tags**: `ffmpeg`, `ai`, `build`

## Context

The `vmaf_pre` FFmpeg filter (added in `ffmpeg-patches/0002-add-vmaf_pre-filter.patch`)
shipped a `parse_device()` helper that only recognized five device strings — `auto`, `cpu`,
`cuda`, `openvino`, and `rocm` — matching the initial state of the `VmafDnnDevice` enum at
the time the filter was authored. Subsequent PRs extended `VmafDnnDevice` in `core/include/libvmaf/dnn.h`
with seven additional values (`openvino-npu`, `openvino-cpu`, `openvino-gpu`, `coreml`,
`coreml-ane`, `coreml-gpu`, `coreml-cpu`), but the patch was not updated in lockstep.
As a result, any `vmaf_pre=device=coreml` or `vmaf_pre=device=openvino-npu` invocation
silently fell through to `AVERROR(EINVAL)` and refused to load, despite the underlying
`libvmaf_dnn` runtime supporting those execution providers. The deep audit (2026-05-15,
finding #14) flagged this as a user-facing correctness bug.

## Decision

Expand `parse_device()` in the patch to map all twelve `VmafDnnDevice` string names that
the main `libvmaf` filter's `tiny_device=` option already accepts, and update the option
description string to list them. No ABI or API change is required; only the patch file and
its in-line description string change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep 5-entry table, document the restriction | Zero code change | Actively misleads users; `libvmaf_dnn` supports all 12 | Unacceptable: ADR-0100 requires docs to match the actual surface |
| Map all 12 in the patch | Consistent with main `libvmaf` filter; zero new runtime dependency | Patch diff grows slightly | Chosen |

## Consequences

- **Positive**: `vmaf_pre=device=coreml-ane`, `vmaf_pre=device=openvino-npu`, and all other
  `VmafDnnDevice` variants work from FFmpeg instead of silently failing.
- **Negative**: None — the additional branches are plain string comparisons; no latency impact.
- **Neutral / follow-ups**: When a new `VmafDnnDevice` value is added to `dnn.h`, the
  patch must be updated in the same PR per CLAUDE.md §12 r14.

## References

- Deep audit finding #14: `.workingdir/AUDIT-DEEP-2026-05-15.md`
- `ffmpeg-patches/0002-add-vmaf_pre-filter.patch`
- `core/include/libvmaf/dnn.h` — `VmafDnnDevice` enum
- `ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch` — reference implementation (12-entry map)
- CLAUDE.md §12 r14 (ffmpeg-patches must stay in sync with public API)
