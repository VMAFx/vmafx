<!-- markdownlint-disable MD013 MD060 -->

# ADR-1197: The threaded flush leaves GPU extractors to their own backend flush

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, sycl, threading, cli, testing

## Context

`vmaf --threads N` failed on every GPU backend, for every `N`, on every input:

```text
libvmaf ERROR context could not be synchronized
exit 234
```

The message is wrong, and that is the first thing worth recording. Instrumenting
`flush_context_cuda()` shows all four driver calls returning success —
`cuCtxPushCurrent=0 cuStreamSynchronize=0 cuCtxSynchronize=0 cuCtxPopCurrent=0` — while
the accumulated `err` is `-22`. The context was healthy throughout; the failure was a
feature extractor's, folded into the same variable as the driver's result and then
reported under the driver's name.

The extractor error came from a duplicate write. `flush_context_threaded()` flushed every
extractor carrying `VMAF_FEATURE_EXTRACTOR_TEMPORAL`, GPU ones included. Its *second*
loop already skips `VMAF_FEATURE_EXTRACTOR_CUDA` deliberately; its first loop did not, and
that asymmetry is the defect. Flushing a temporal GPU extractor there ran the extractor's
tail-batch drain before the pending boundary collect in `flush_context_cuda()`, so the
later collect re-emitted an index that was already written and the feature collector
returned `-EINVAL`.

The duplicate write was not the only consequence, and this is why the fix cannot simply
suppress it. Draining the tail before the boundary collect also emits the last
batch-boundary frame without the `min()` against the following frame that `motion2` and
`motion3` are defined by. On the 48-frame Netflix pair, frame 39's
`integer_motion2_mmxv_18` becomes `4.382255` instead of `3.724278`, and pooled VMAF
becomes `82.823778` instead of `82.814059`. Every other feature is bit-identical, and CPU
serial, CPU threaded and CUDA serial all agree on the correct value. A fix that only
skipped the redundant collect would therefore have turned a loud crash into a silently
wrong score — strictly worse.

`flush_context_sycl()` has the same shape and never had a thread-pool guard in any form,
so SYCL failed identically.

The blast radius is larger than the CLI flag. `testdata/bench_all.sh` hard-codes
`--threads 1`, so its GPU rows were masked failures for as long as this existed.

## Decision

`flush_context_threaded()` will not flush GPU extractors at all. Its first loop now skips
anything flagged `VMAF_FEATURE_EXTRACTOR_CUDA` or `VMAF_FEATURE_EXTRACTOR_SYCL`, matching
what its second loop already did, and `flush_context_cuda()` / `flush_context_sycl()` own
those extractors in both threaded and serial mode. Each therefore runs collect-then-flush
in exactly the order the serial path uses, which is the order that produces correct
`motion2` / `motion3` values. The thread-pool special case in `flush_context_cuda()` is
deleted, because there is no longer a mode in which the threaded flush has already
touched a GPU extractor.

Separately, `flush_context_cuda()` keeps the extractor result and the driver result in
distinct variables and reports them distinctly. "Context could not be synchronized" now
means the context could not be synchronized.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Threaded flush skips GPU extractors; backend flush owns them (**chosen**) | Threaded output is bit-identical to serial on both backends at every `N`; restores the symmetry the second loop already had; deletes a special case rather than adding one | Slightly more work in the backend flush path when threaded | — |
| Extend the existing guard to cover the collect as well as the flush | One-line change; stops the crash | **Measured wrong**: `--threads N` then returned `82.823778` against serial `82.814059`, with frame 39's motion2 off by `+0.66`. Trades a crash for a silently wrong score | Rejected on evidence, not on principle — it was built and run |
| Treat a duplicate-write `-EINVAL` as non-fatal in the flush path | Smallest diff | Suppresses the symptom and keeps the wrong frame-39 value; also blinds a genuinely useful error | Hides a correctness bug |
| Make the feature collector idempotent on repeat writes | Fixes this and any future double-flush | A silent overwrite policy in shared collector state is a much larger semantic change, and "last writer wins" is not obviously right | Disproportionate, and out of scope for a crash fix |
| Reject `--threads` on GPU backends | Trivially honest | Removes a working user surface to avoid fixing it; the threaded path works correctly once ownership is fixed | Removing a user surface is not a fix |

## Consequences

- **Positive**: `vmaf --threads N` works on CUDA and SYCL and is bit-identical to the
  serial run — verified at `N` = 1, 2, 4, 8, per-frame across all 48 frames and every
  metric key. GPU errors during flush no longer misreport as context-synchronization
  failures.
- **Negative**: none measured. The 4 GPU parity tests that fail in this tree
  (`test_cuda_float_adm_parity`, `test_cuda_psnr_hvs_parity`,
  `test_cuda_ssimulacra2_parity`, `test_vmaf_cuda_gpumask`) fail identically on unmodified
  `origin/master` and are unrelated.
- **Neutral / follow-ups**: `testdata/bench_all.sh` pins `--threads 1`; its GPU rows were
  masked failures and its recorded GPU numbers should be re-taken now that the flag
  works. Tracked with the state.md row for this bug.

## References

- req: found while validating epic #1246's GPU gates; the empirical `--threads` reproducer
  came out of the epic #1245 benchmark pass in
  [PR #1334](https://github.com/VMAFx/vmafx/pull/1334).
- `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06` in [state.md](../state.md).
- [ADR-0845](0845-cuda-motion-launch-overhead.md) — the batched motion collect whose boundary
  semantics this preserves.
