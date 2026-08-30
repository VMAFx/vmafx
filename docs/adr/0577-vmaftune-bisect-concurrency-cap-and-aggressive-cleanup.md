<!-- markdownlint-disable MD060 -->
# ADR-0577: vmaf-tune bisect decode concurrency cap and aggressive workdir cleanup

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `compare`, `bisect`, `disk-space`, `concurrency`, `fork-local`

## Context

The BBB v13 1080p `vmaf-tune compare` run failed every CPU cell with ENOSPC
(rc=228). The root cause: `compare` dispatches each codec bisect to a thread
pool. With three codecs running concurrently, each bisect materialised the
110 GB BBB 1080p reference YUV decode independently and in parallel, producing a
330 GB peak on the 420 GB `/probes` volume. There was no mechanism to serialise
or cap the number of concurrent reference-YUV decode operations, and the
per-iteration encoded MKVs and decoded YUVs accumulated until end-of-run rather
than being cleaned up immediately.

The existing ADR-0549 preflight check fires before the bisect starts; it does
not protect against mid-run ENOSPC when multiple threads decode simultaneously
or when scratch files accumulate across iterations.

## Decision

Three complementary fixes ship together:

1. **Decode concurrency cap (`--max-concurrent-decodes N`, default 1).**
   A `threading.Semaphore` is installed at the module level in
   `vmaftune.bisect` and acquired before each reference-YUV decode inside
   `bisect_target_vmaf`. The default (serial decodes) caps peak disk usage
   to one reference YUV decode at a time regardless of thread-pool size.
   The CLI flag is wired on `compare`, `ladder`, and `tune-per-shot`;
   operators with large `--workdir` volumes can raise `N` to trade peak
   disk for throughput.

2. **Aggressive workdir cleanup.**
   After each bisect iteration completes (encode + decode + score for one
   CRF probe), the iteration's encoded `.mkv` and decoded distorted
   `.yuv` are deleted immediately. This behaviour was already implemented
   in `_encode_and_score`; the new addition is the per-bisect cleanup of
   the decoded reference YUV in `bisect_target_vmaf`'s `finally` block.
   Previously the reference YUV persisted until the bisect completed; now
   it is deleted as soon as the bisect for that (codec, target) pair
   finishes, before the next codec's bisect acquires the semaphore.

3. **Mid-run disk-space monitoring.**
   Before each iteration's decode, `bisect_target_vmaf` re-checks
   `shutil.disk_usage(workdir).free`. The guard requires 2x the
   estimated YUV size to be free (the extra headroom covers the encoded
   MKV coexisting with the decoded YUV). On failure the bisect returns
   `BisectResult(ok=False, error=...)` with a context string naming the
   codec and target VMAF, replacing the opaque ffmpeg rc=228.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raise the `/probes` volume size | Simple operational fix | Does not scale; next source or next run hits the same wall | Not a code fix; symptom not cause |
| Single shared workdir per codec (share the reference YUV across all targets) | One decode per codec instead of one per (codec, target) | Complicates lifecycle; reference YUV cannot be deleted until all targets for that codec complete | The semaphore + per-bisect cleanup already achieves the same peak (one decode at a time) without shared state |
| Delete reference YUV between codec iterations only (not per bisect) | Slightly fewer deletions | Does not help when a single bisect already ENOSPC on the first decode | Too coarse |
| Reduce default thread-pool workers | Simpler | Slows encoder runs which are not disk-bound | Worsens throughput without proportional disk benefit |

## Consequences

- **Positive**: Peak workdir disk usage drops from `N_codecs x yuv_size`
  (e.g. 330 GB for 3 codecs x 110 GB BBB) to `yuv_size` (110 GB) at the
  default `--max-concurrent-decodes 1`. The mid-run check surfaces ENOSPC
  with a structured `BisectResult(ok=False)` error instead of a partial-
  write and corrupt JSON.
- **Negative**: With `--max-concurrent-decodes 1`, reference-YUV decodes
  serialise across threads. On a typical 110 GB BBB source at ~2 GB/s
  sustained disk read this adds ~55 s per codec; on the production
  `/probes` NVMe array the wall-time impact is negligible compared to the
  encode and score time.
- **Neutral / follow-ups**:
  - The `--max-concurrent-decodes` flag on `ladder` is accepted but
    currently a no-op (ladder uses corpus sweeps, not bisect decodes).
    It is wired for operator consistency and future use.
  - `set_decode_semaphore` and `DEFAULT_MAX_CONCURRENT_DECODES` are
    exported from `vmaftune.bisect.__all__` so library callers can
    configure the cap before spawning a thread pool.

## References

- ADR-0549: workdir relocation and preflight disk-space check (the ADR-0577
  mid-run check extends this to intra-run space monitoring).
- `req`: the BBB v13 1080p compare failed every CPU cell with ENOSPC
  because the thread pool ran 3 concurrent bisects, each materialising the
  110 GB reference YUV decode in parallel, overflowing the 420 GB /probes
  volume.
- `req`: fix scope: `--max-concurrent-decodes N` CLI flag (default 1);
  aggressive workdir cleanup (per-iteration MKV and YUV, per-bisect ref
  YUV); mid-run disk-space monitoring with 2x headroom before each decode.
