---
name: refresh-ffmpeg-patches
description: Refresh the full FFmpeg patch series against the configured stable release or the latest stable released tag, retaining conflict diagnostics.
---
# /refresh-ffmpeg-patches

Root `build-config.env` owns maintained FFmpeg remote and release tag.
Same implementation as local hooks and CI:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh
python3 scripts/ci/ffmpeg_patch_stack.py --check
```

- Explicit upstream update: use `--refresh --latest`.
- Selects highest stable released tag; excludes master, development, RC and
  snapshot refs.
- Daily CI performs discovery -> retains proposed diff.
- Ordinary commits and PRs replay reviewed release only.
- Helper fetches into disposable storage.
- Applies each `series.txt` entry in order.
- Writes canonical patches/configuration mirrors only after complete replay and
  rebase succeed.
- Do not reset or clean existing checkouts.
- Failure -> inspect printed diagnostics directory; resolve integration source
  deliberately, replay complete series.
- Later patches depend on earlier ones -> continuing after failed patch is not
  a valid series check.
- Review generated diff and validation before commit within user authorized
  scope.
- Patch refresh cannot invent semantics of new public API.
- See
  [FFmpeg patch
  automation](../../../docs/development/ffmpeg-patch-automation.md)
  and [ADR-1240](../../../docs/adr/1240-ffmpeg-release-patch-lifecycle.md).
