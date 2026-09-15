# Refreshing the in-tree FFmpeg patch series

The current procedure is documented in [FFmpeg patch automation](ffmpeg-patch-automation.md).
The root `build-config.env` owns the upstream remote and stable release tag;
`ffmpeg-patches/series.txt` defines the cumulative patch order. Historical
refreshes in [ADR-0118](../adr/0118-ffmpeg-patch-series-application.md) and
[ADR-0277](../adr/0277-ffmpeg-patches-refresh-2026-05-04.md) describe earlier
baselines, not the current release selection.

## Refresh after a libvmaf surface change

Run from the repository root:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh \
  --output-dir .workingdir2/cache/ffmpeg-patch-stack/manual-refresh
python3 scripts/ci/ffmpeg_patch_stack.py --check \
  --output-dir .workingdir2/cache/ffmpeg-patch-stack/manual-check
```

The tool fetches the configured release into a disposable checkout and replays
all patches in series order. Review the regenerated diff, update the relevant
[rebase notes](../rebase-notes.md) and changelog fragment, and include the patch
changes with the libvmaf surface change. Replay failure must be resolved before
refresh can replace the maintained files. The automation guide explains failure
receipts, recovery backups and local hook behavior.

## Move to a newer release

The scheduled updater discovers the latest stable release and retains a proposed
refresh artifact for review. Ordinary hooks and pull-request checks use the
configured release. See [release discovery](ffmpeg-patch-automation.md#continuous-checks-and-release-discovery)
for the schedule, manual invocation and artifact contents.

## See also

- [Using FFmpeg with libvmaf](../usage/ffmpeg.md)
- [FFmpeg patch lifecycle decision](../adr/1240-ffmpeg-release-patch-lifecycle.md)
- [Fork rebase notes](../rebase-notes.md)
