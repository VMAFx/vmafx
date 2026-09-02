# Refreshing the In-Tree FFmpeg Patch Series

The fork's FFmpeg integration is a stack of patches under
`ffmpeg-patches/series.txt`, applied on top of upstream FFmpeg `n8.1`
(per [ADR-0118](../adr/0118-ffmpeg-patch-series-application.md)).
The stack drifts as upstream FFmpeg moves and as fork-side
libvmaf surfaces change ([CLAUDE.md §12 r14](../../CLAUDE.md)).

**Refreshing** the stack means:

1. Replay the patches against a clean `n8.1` checkout to verify the
   stack still applies cleanly.
2. Resolve any conflicts that fall out of upstream churn.
3. Re-export the patch files from the rebased branch.
4. Land the refresh in tree alongside an entry in
   [`docs/rebase-notes.md`](../rebase-notes.md).

The most recent refresh is documented in
[ADR-0277](../adr/0277-ffmpeg-patches-refresh-2026-05-04.md). The
`/refresh-ffmpeg-patches` skill (declared in
[`CLAUDE.md` §7](../../CLAUDE.md)) automates the replay step.

## Preconditions

- Keep a clean FFmpeg checkout available at the upstream tag the fork
  currently targets (`n8.1` unless a newer ADR says otherwise).
- Start from a clean VMAF branch; refresh PRs should contain the patch
  stack delta and the required notes, not unrelated source changes.
- Read [`docs/rebase-notes.md`](../rebase-notes.md) before resolving
  conflicts. Several patch hunks mirror fork-local libvmaf API surfaces.

## Refresh Steps

1. Reset the FFmpeg checkout to the target tag:

   ```bash
   git -C /path/to/ffmpeg-8 reset --hard n8.1
   ```

2. Apply the current series cumulatively:

   ```bash
   for p in ffmpeg-patches/000*-*.patch; do
       git -C /path/to/ffmpeg-8 am --3way "$PWD/$p" || break
   done
   ```

3. Resolve conflicts inside the FFmpeg checkout and continue `git am`.
4. Export the refreshed commits back into `ffmpeg-patches/` in series
   order.
5. Update `ffmpeg-patches/series.txt` if the patch list changed.
6. Add a changelog fragment and a rebase-note entry describing the
   refreshed surface.

## Verification

The correct gate is a *cumulative-state* replay, not per-patch
`git apply --check` — patches `0002` … `0006` build on each other
and standalone-apply cleanly only against the cumulative state from
earlier patches:

```bash
git -C /path/to/ffmpeg-8 reset --hard n8.1
for p in ffmpeg-patches/000*-*.patch; do
    git -C /path/to/ffmpeg-8 am --3way "$p" || break
done
```

## See also

- [`docs/usage/ffmpeg.md`](../usage/ffmpeg.md) — the user-facing
  FFmpeg-with-libvmaf doc.
- [ADR-0118](../adr/0118-ffmpeg-patch-series-application.md) — the
  series.txt model.
- [ADR-0277](../adr/0277-ffmpeg-patches-refresh-2026-05-04.md) —
  most recent refresh log.
- [`docs/rebase-notes.md`](../rebase-notes.md) — fork-wide rebase
  log.
