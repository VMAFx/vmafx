<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1090: Fix CUDA stream and event leaks on init error paths

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `security`, `testing`

## Context

A systematic audit of `core/src/cuda/` and `core/src/feature/cuda/` identified
five functions whose error paths leaked CUDA resources:

1. **`picture_cuda.c:vmaf_cuda_picture_alloc`** — all failure targets pointed to
   a single `fail` label that only freed the `VmafPicturePrivate` heap block.
   Any `cuEventCreate`, `cuMemAllocPitch`, or `vmaf_ref_init` failure left the
   CUstream (`priv->cuda.str`) and zero to two CUevents
   (`priv->cuda.ready`, `priv->cuda.finished`) live in the driver for the
   duration of the process.

2. **`integer_vif_cuda.c:init_fex_cuda`** — same pattern: `cuEventCreate` or
   `cuModuleLoadData` failure jumped to `fail`, which popped the context but
   did not call `cuStreamDestroy` or `cuEventDestroy`.

3. **`integer_adm_cuda.c:init_fex_cuda`** — `cuEventCreate` (×3),
   `cuModuleLoadData` (×4), and all `cuModuleGetFunction` calls shared the
   same bare `fail` label that left `s->str`, three events, and up to four
   module handles unreleased.

4. **`integer_motion_cuda.c:init_fex_cuda`** — `cuEventCreate` and
   `cuModuleLoadData`/`cuModuleGetFunction` failures left `s->str` and
   `s->event` live.

5. **`ssimulacra2_cuda.c:init_fex_cuda`** — all module and function-lookup
   failures shared `fail`, leaking `s->str` and any partially loaded PTX
   module backing store (several hundred KB per module, per NVIDIA documentation
   and `compute-sanitizer --tool memcheck` observation).

In production use these paths are rarely hit (they require a broken CUDA
installation or OOM), but in integration test harnesses that repeatedly
init/close VMAF contexts the leaks accumulate and are caught by
`compute-sanitizer`.

## Decision

Replace each function's single shared `fail` label with a chain of graduated
cleanup labels (`fail_after_stream`, `fail_after_event`, `fail_after_module`,
`fail_after_events`, etc.) so that each label frees only the resources
allocated above it and falls through to the next. The pattern mirrors the
existing fix in `common.c` (committed as part of the ADR-0960 round-25 audit)
where `cuStreamDestroy` was added to the `cuCtxPopCurrent` failure path.

No behaviour change on the success path; no score change; no public API change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single `cleanup:` label that null-checks every handle | Simpler label set | Extra null-checks on success path; mixes concerns | Graduated chain is the CERT MEM12-C recommended pattern and matches the existing style in `common.c` |
| Leave leaks; document as won't-fix | Zero code change | Leaks accumulate in sanitizer runs and long-lived server processes | Resources must be released — CERT MEM31-C |

## Consequences

- **Positive**: `compute-sanitizer --tool memcheck` reports no leaked CUDA
  handles on error-path injection tests. Consistent with the cleanup idiom
  already used in `common.c`.
- **Negative**: Each fixed function gains 10–25 lines of cleanup labels,
  increasing vertical size.
- **Neutral**: No change to the normal (success) code path or to scores.

## References

- ADR-0960: prior round-25 audit that fixed `cuStreamDestroy` leak in
  `common.c` init paths.
- CERT MEM31-C: free dynamically allocated memory when no longer needed.
- CERT MEM12-C: do not return if realloc() fails — generalised: use cleanup
  chains rather than a single shared error target.
- Files changed: `core/src/cuda/picture_cuda.c`,
  `core/src/feature/cuda/integer_vif_cuda.c`,
  `core/src/feature/cuda/integer_adm_cuda.c`,
  `core/src/feature/cuda/integer_motion_cuda.c`,
  `core/src/feature/cuda/ssimulacra2_cuda.c`.
