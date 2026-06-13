<!-- markdownlint-disable MD013 MD060 -->
# ADR-1107: Per-extractor `prev_ref` in the threaded batch path (multi-PREV_REF starvation fix)

- **Status**: Accepted
- **Date**: 2026-06-13
- **Deciders**: Lusoris
- **Tags**: `core`, `threading`, `feature-extractor`, `refcount`, `bug-fix`, `fork-local`

## Context

`threaded_read_pictures_batch` / `threaded_extract_batch_func`
(`core/src/libvmaf.c`) feed the previous reference frame to
`VMAF_FEATURE_EXTRACTOR_PREV_REF` extractors under `--threads N`. The
batch took one refcounted snapshot of `prev_ref` (`f->prev_ref`) and, for
each PREV_REF extractor in the loop, **struct-copied** it into that
extractor's thread-private context:

```c
if (f->prev_ref.ref)
    td->fex_ctx[i]->fex->prev_ref = f->prev_ref;   /* alias, no extra count */
```

The per-extractor cleanup then unref'd the context's `prev_ref` (consuming
the single snapshot count via the alias) and **zeroed the shared
`f->prev_ref`** so the final `unref:` block would not double-free.

That logic is correct for **one** PREV_REF extractor per batch, which was
the only case exercised by `test_thread_safety_batch.c` (motion alone).
With **two** PREV_REF extractors it breaks: the first extractor's swap
consumes the snapshot count and the loop zeroes `f->prev_ref`, so the
second extractor sees `f->prev_ref.ref == NULL` and its `extract()`
returns `-EINVAL` on every frame ("problem with feature extractor
motion_v2"). `vmaf_read_pictures` then fails and no motion_v2 features are
produced.

Requesting `motion_v2` always co-schedules `motion` (two PREV_REF
extractors), so **every** multi-threaded extraction involving `motion_v2`
failed 100% — including the K150K corpus feature extraction that feeds the
one-shot model retrain (`extract_k150k_features.py` runs `--threads 8`
with both motion features). Single-threaded runs (the Netflix golden
gate's path) were unaffected, which is why CI stayed green while the
threaded path was broken. The earlier fixes to this same function
([ADR-1072](1072-threaded-batch-prev-ref-leak.md) refcount leak,
[ADR-1073](1073-threaded-flush-initialized-flag.md) flush gate) did not
cover the multi-extractor case.

## Decision

Give each PREV_REF extractor its **own** counted reference and keep the
shared snapshot live for the whole batch:

1. Replace the struct-copy with `vmaf_picture_ref(&td->fex_ctx[i]->fex->prev_ref, &f->prev_ref)`
   so each extractor owns one count, balanced by its own PREV_REF swap.
2. Stop zeroing the shared `f->prev_ref` inside the per-extractor cleanup;
   the snapshot is released exactly once in the loop's `unref:` block.

Refcount balance on frame N-1's `VmafRef`: `+1` snapshot, then per
extractor `+1` ref `−1` swap-unref (net 0), then `−1` final unref → 0. No
leak, no double-free, no use-after-free; the per-frame `prev_ref` is
re-propagated each iteration so no state crosses frames. Scores are
unchanged (threaded == single-threaded, verified on KoNViD-150K).

A regression test (`test_batch_two_prev_ref_extractors` in
`test_thread_safety_batch.c`) registers motion + motion_v2 under
`n_threads=4` and asserts both `motion_sad_score` and `motion_v2_sad_score`
are produced; it fails before this fix ("EOS failed") and passes after.

## Alternatives considered

- **Add `VMAF_FEATURE_EXTRACTOR_TEMPORAL` to motion_v2** so the ctx pool
  pins it to a single slot and the batch loop skips it. Rejected:
  inconsistent with motion (also PREV_REF, no TEMPORAL) and would not fix
  any other pair of PREV_REF extractors; the root cause is the shared
  snapshot, not the flag.
- **Workaround in `extract_k150k_features.py`** (separate motion /
  motion_v2 invocations). Rejected by operator direction: fix the libvmaf
  bug properly — it affects any caller running both motion features with
  `--threads`, not just our extraction.

## Consequences

- **Positive**: Threaded extraction with any number of PREV_REF extractors
  works; the K150K retrain extraction is unblocked; a regression test
  guards the path (TSan/ASan-eligible).
- **Negative**: One extra `vmaf_picture_ref` per PREV_REF extractor per
  frame (negligible; a refcount bump).
- **Neutral / follow-ups**: The CUDA `motion_v2`/`motion` co-scheduling
  also emits "cannot be overwritten" warnings + a context-sync error on
  some clips (seen in the CUDA smoke); that is a separate GPU-path issue
  tracked in `docs/state.md` and does not affect the CPU retrain path.

## Supply-chain impact

- **New dependencies**: none.
- **Removed dependencies**: none.
- **Build-time fetches**: none.

## References

- Same-function precedent: [ADR-1072](1072-threaded-batch-prev-ref-leak.md),
  [ADR-1073](1073-threaded-flush-initialized-flag.md);
  thread-private `prev_ref` invariant [ADR-0795](0795-threaded-prev-ref-thread-private.md),
  pool-exhaustion [ADR-1051](1051-threaded-prev-ref-unref.md).
- Source: surfaced by the one-shot-retrain K150K extraction smoke test
  (motion + motion_v2 under `--threads 8` failed 100%); operator chose the
  in-libvmaf fix over a script workaround.
