---
name: refresh-ffmpeg-patches
description: Refresh the full FFmpeg patch series against the configured stable release or the latest stable released tag, retaining conflict diagnostics.
---

# /refresh-ffmpeg-patches

The root `build-config.env` owns the maintained FFmpeg remote and release tag.
Use the same implementation as local hooks and CI:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh
python3 scripts/ci/ffmpeg_patch_stack.py --check
```

When explicitly updating upstream, use `--refresh --latest`. This selects the
highest stable released tag, excluding master, development, RC and snapshot
refs. Daily CI already performs this discovery and retains a proposed diff.
Ordinary commits and PRs replay only the reviewed release.

The helper fetches into disposable storage, applies every `series.txt` entry
in order and writes canonical patches/configuration mirrors only after the
complete replay and rebase succeed. Do not reset or clean existing checkouts.
If it fails, inspect the printed diagnostics directory; resolve the integration
source deliberately and replay the complete series. Later patches depend on
earlier ones, so continuing after a failed patch is not a valid series check.

Review the generated diff and validation before committing within the user's
authorized scope. Patch refresh cannot invent the semantics of a new public API.
See [FFmpeg patch automation](../../../docs/development/ffmpeg-patch-automation.md)
and [ADR-1240](../../../docs/adr/1240-ffmpeg-release-patch-lifecycle.md).
