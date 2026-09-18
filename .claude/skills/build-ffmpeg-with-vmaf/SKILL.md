---
name: build-ffmpeg-with-vmaf
description: Build FFmpeg from the configured released tag with the complete VMAFx patch series and smoke-test its filters in the dev container.
---
# /build-ffmpeg-with-vmaf

- Native integration path: use dev-MCP container described in `AGENTS.md`.
- Confirm installed libvmaf version matches source under test; rebuild
  when required by container freshness rule.

Validate patch series:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --check
```

Run build helper in container with worktree mounted as repo root:

```bash
bash ffmpeg-patches/test/build-and-run.sh
```

- Helper uses `FFMPEG_REMOTE` and `FFMPEG_TAG` from `build-config.env`.
- Applies every patch in `series.txt`, in listed order.
- Builds ffmpeg against installed libvmaf.
- Checks `libvmaf` tiny-model option and `vmaf_pre` filter.
- Set `VMAF_PREFIX` for nonstandard libvmaf installation.
- Missing prerequisites -> exit 77 = unavailable result, not pass or fail.
- Default source checkout = disposable.
- Explicit `FFMPEG_SRC` must be new path; existing checkouts rejected intact.
- `KEEP_BUILD=1` retains successful build; failures remain available for
  diagnosis.
- `FFMPEG_SHA` can select another stable released tag; development and
  arbitrary commit refs rejected.
- Empty or failed series = not valid integration result.
- Report source revision, installed libvmaf version, applied patch count,
  actual smoke results.

See:
[FFmpeg patch automation](../../../docs/development/ffmpeg-patch-automation.md).
