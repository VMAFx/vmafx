<!-- markdownlint-disable MD013 -->
# Research-0094: `integer_motion_v2` `flush()` dictionary leak — round-7 stability audit

**Date**: 2026-05-10
**Branch**: fix/motion-v2-flush-dict-leak
**Companion ADR**: none — single-path fix, no alternative design space

## Summary

Round-7 long-loop ASan scan (Pattern D: all CPU features, threaded, single clip) reported
378 bytes leaked per run, traced to `vmaf_feature_name_dict_from_provided_features` called
from `flush()` in `integer_motion_v2.c`.

## Root-cause trace

In the threaded dispatch path (`n_threads > 1`):

1. Per-frame extraction uses pool instances acquired via `vmaf_fex_ctx_pool_aquire`. Each
   pool context gets `extract()` called, which sets `s->feature_name_dict` in that context's
   private state.
2. `flush_context_threaded` (libvmaf.c:1244–1257) calls `fex->flush(fex, ...)` on
   `rfe.fex_ctx[i]->fex`, which is the **registered** context — never a pool instance.
3. The registered context was never passed through `vmaf_feature_extractor_context_init`,
   so `fex_ctx->is_initialized = false`.
4. `flush()` checks `!s->feature_name_dict` (which is NULL in the registered context),
   allocates a new dict (378 bytes across 8 allocations), and stores it in `s->feature_name_dict`.
5. At teardown, `vmaf_feature_extractor_context_close()` guards on `!fex_ctx->is_initialized`
   (line 536) and returns `-EINVAL` without calling `close_fex()`, so the dict is never freed.

## Affected pattern

Only `integer_motion_v2.c` is affected among CPU fexes because it is the only one that
re-allocates the dict inside `flush()` (the `!s->feature_name_dict` guard at line 379).
All other CPU fexes that use `feature_name_dict` allocate it unconditionally in `extract()`
and do not re-create it in `flush()`.

## Leak characterisation

- 378 bytes per scoring run (constant, not per-frame): 16 + 128 + 117 + 117 bytes.
- Triggered whenever `--feature motion_v2` is used with `--threads N` (N > 1).
- Cumulative in a long-running encoder process (e.g., per-title VMAF scoring).
- Not triggered in `--threads 1` / serial path (registered fex IS used for extract there).

## Fix

Track whether the dict was NULL at `flush()` entry via `const bool dict_locally_owned`.
When true, free the dict before every return path inside `flush()`. When false (pool-instance
or serial path where `extract()` already allocated the dict), leave it for `close_fex()`.

## Verification

- ASan Pattern D (all CPU features + motion_v2, threads=4, single clip): 0 bytes leaked.
- ASan Pattern C (50× init/destroy, threads=2, adm+vif+motion+motion_v2): 0 bytes leaked.
- `meson test -C build-leak`: 53/53 pass.
- TSan Pattern B (8 threads, 100-frame, all CPU features): 0 data races.

## Additional findings (deferred as architectural)

Two heap-buffer-overflows were identified during Pattern E (pathological inputs) and are
deferred as architectural scope:

**Finding E-1: `edge_8` / `edge_16` overflow for frames shorter than `radius+1 = 3` px.**
The 5-tap mirror filter (`filter_width=5`, `radius=2`) in `integer_motion.{c,h}` and all
SIMD paths computes reflected tap indices assuming `height >= radius + 1 = 3`. For `height
< 3` (e.g., 1×1 or 2×N frames), the reflection formula `height - (i_tap - height + 2)`
underflows to a negative signed value, which as an array index produces a heap read past
the allocation. Reproducer: `--width 1 --height 1 --feature motion --threads 1`.
Scope: scalar + AVX2 + AVX-512 paths all affected. Fix requires a minimum-dimension guard
(`h < filter_width/2 + 1 → -ENOTSUP`) in `integer_motion.c:init()` and corresponding
guards in motion_v2 and float_motion. Not shipped in this PR.

**Finding E-2: `scale_chroma_planes` overflow for odd-height YUV 4:2:0.**
`ciede.c:scale_chroma_planes` iterates over all `out->h[1]` luma-height rows but reads
chroma via `in_buf` advancing by `in->stride[1]` every two rows. For odd-height frames
(e.g., 577×323), `in->h[1] = h >> 1 = 161` (floor), but the loop reaches chroma row 161
(0-indexed) on the last luma row, which is one past the allocation. Root cause is that
`picture.c:picture_compute_geometry` uses `h >> ss_ver` (floor division) for chroma height
instead of `(h + ss_ver) >> ss_ver` (ceiling). Affects any feature that iterates all luma
rows while indexing chroma by halving the row coordinate. Reproducer:
`--width 577 --height 323 --feature ciede --threads 4`. Architectural scope; not shipped.
