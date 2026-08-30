<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1051: Port upstream batch-threading + picture-pool defaults (dff4082b + 46d3a154)

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `upstream-port`, `scoring`, `threading`, `correctness`

## Context

Netflix upstream shipped two companion commits on 2026-05-08 that make batch-threaded
execution and CPU picture-pool allocation the unconditional default in `libvmaf.c`:

- `dff4082b` — "libvmaf: enable batch threading by default": removes all
  `#ifdef VMAF_BATCH_THREADING` guards, making the batch-thread path the only
  threaded execution path. Deletes the old `threaded_extract_func` /
  `threaded_read_pictures` non-batch path.
- `46d3a154` — "libvmaf: enable picture pool by default": removes all
  `#ifdef VMAF_PICTURE_POOL` guards; `picture_pool.c` becomes an unconditional
  source. The CLI `fetch_picture` path is simplified to always use the pool.

The fork's `core/src/libvmaf.c` still had 9 `#ifdef VMAF_BATCH_THREADING` guards
(the upstream audit classified these as P0 scoring divergence). The fork had no
`VMAF_PICTURE_POOL` guards remaining in `libvmaf.c` — the picture pool was already
unconditional there — but the meson.build had a stale comment referring to the old
optional-flag framing.

## Decision

Port both upstream commits to `core/src/libvmaf.c`:

1. Remove all `#ifdef VMAF_BATCH_THREADING` guards. The batch-thread functions
   (`BatchThreadData`, `batch_thread_data_free`, `threaded_extract_batch_func`,
   `threaded_read_pictures_batch`) become unconditional. The old non-batch
   `threaded_read_pictures` path is removed (it was already dead in the fork
   because the VMAF_BATCH_THREADING flag was always set at compile time in the
   CI build).
2. Update the stale meson.build comment referencing the old experimental-flag
   framing; no functional build change needed (picture_pool.c was already
   unconditionally compiled per ADR-0994).

The fork's CUDA/SYCL/HIP paths are unaffected — they branch on
`VMAF_FEATURE_EXTRACTOR_CUDA`, `VMAF_FEATURE_EXTRACTOR_SYCL`, and `HAVE_CUDA`
flags which remain orthogonal to the threading model.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `#ifdef VMAF_BATCH_THREADING` guards | No change risk | Fork diverges from upstream; scheduling semantics differ for multi-threaded callers; P0 classification in upstream audit | Not acceptable for a P0 divergence |
| Port verbatim via `cherry-pick` | Cleaner diff | The upstream diff touches `libvmaf/src/` paths; the fork moved to `core/src/`. Direct cherry-pick fails on path mismatch. | Adapted port chosen |

## Consequences

- **Positive**: Fork now matches upstream threading semantics; the old dead
  `threaded_read_pictures` path is eliminated; `#ifdef` count in `libvmaf.c`
  reduced.
- **Negative**: The non-batch threaded path is removed; any caller that relied
  on the non-batch per-extractor thread pool (behind the old `#ifndef
  VMAF_BATCH_THREADING` branch) will now use the batch path. In practice the
  CI build always had `VMAF_BATCH_THREADING` set, so this is a no-op for all
  tested configurations.
- **Neutral / follow-ups**: The five-frame-window motion stub (`-ENOTSUP` in
  `integer_motion.c`) remains as a separate deferred item (ADR-0337); this PR
  does not change that.

## References

- Upstream commit `dff4082b263f7e71799d9b5f338bc6cec6d76792` (Netflix/vmaf, 2026-05-08).
- Upstream commit `46d3a154561ed4a969ed99f60b616abc6e6c73d9` (Netflix/vmaf, 2026-05-08).
- Upstream sync audit report, 2026-06-04 (priority: P0).
