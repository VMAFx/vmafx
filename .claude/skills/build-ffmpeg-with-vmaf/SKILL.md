---
name: build-ffmpeg-with-vmaf
description: Build FFmpeg from the configured released tag with the complete VMAFx patch series and smoke-test its filters in the dev container.
---

# /build-ffmpeg-with-vmaf

Use the dev-MCP container described in `AGENTS.md` for native integration work.
Confirm that its installed libvmaf matches the source being tested and rebuild
it when required by the container freshness rule.

First validate the full patch series with:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --check
```

Then run the build helper inside the container with the worktree mounted as
its repository root:

```bash
bash ffmpeg-patches/test/build-and-run.sh
```

The helper uses `FFMPEG_REMOTE` and `FFMPEG_TAG` from `build-config.env`, applies
`series.txt` in order, builds against the installed libvmaf, and checks the
`libvmaf` tiny-model option and `vmaf_pre` filter. Set `VMAF_PREFIX` for a
nonstandard libvmaf installation. Missing prerequisites return exit 77 and
are an unavailable result, not a pass.

The default source checkout is disposable. An explicit `FFMPEG_SRC` must be a
new path; existing checkouts are rejected intact. `KEEP_BUILD=1` retains a
successful build, and failures remain available for diagnosis. `FFMPEG_SHA`
can select another stable released tag; development and arbitrary commit refs
are rejected. An empty or failed series is not a valid integration result.

Report the source revision, installed libvmaf version, applied patch count and
actual smoke results. See [FFmpeg patch automation](../../../docs/development/ffmpeg-patch-automation.md).
