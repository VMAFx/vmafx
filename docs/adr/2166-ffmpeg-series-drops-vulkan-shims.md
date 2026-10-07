<!-- markdownlint-disable MD013 MD060 -->
# ADR-2166: The FFmpeg series drops the Vulkan no-op shims

- **Status**: Proposed
- **Date**: 2026-10-07
- **Supersedes**: [ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md)
- **Deciders**: maintainer (FFmpeg audit request, RC4 WP15); RC4 WP15
- **Tags**: rc4, ffmpeg, build, cleanup

## Context

[ADR-0726](0726-drop-vulkan-backend.md) removed the Vulkan backend.
[ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md) kept patches `0004`
(`vulkan_device` on the `libvmaf` filter) and `0006` (`libvmaf_vulkan`) in the
series as no-op shims, because the hunks of nine later patches (`0005`, `0008`,
`0010` to `0014`, `0016`, `0020`) had Vulkan lines in their context and a
series without the two patches would not replay. The audit
([Research-2166](../research/2166-ffmpeg-audit-2026-10-07.md)) measured what
the shims cost: 601 patch lines, 80 Vulkan lines in `vf_libvmaf.c`, the
Vulkan hunks in nine more patches, a pkg-config probe of the removed
`libvmaf/libvmaf_vulkan.h` on every FFmpeg configure, a registered but never
built filter in `allfilters.c`, and one `-Wunused-parameter` per compiler that
no configuration can remove. `CONFIG_LIBVMAF_VULKAN` and
`CONFIG_LIBVMAF_VULKAN_FILTER` are false on every build.

## Decision

1. **Patches `0004` and `0006` are removed**, and every Vulkan line is removed
   from the patches that carried it (`0005`, `0008`, `0010` to `0014`, `0016`,
   `0020`), by replaying the series without the two commits and resolving each
   conflict by hand. The remaining patches keep their numbers (`series.txt`
   leaves the gap and says why) so references to them stay valid; WP10's list
   of patches to retire is unchanged.
2. **No behaviour changes for any build that works today.** The check is
   semantic, not textual: every added or removed line of every replayed patch
   equals the old patch's apart from Vulkan content, the CUDA selector
   (`copy_picture_data()` now takes the context it always needed for the CUDA
   pool) compiles and scores on the RTX 4090 (`libvmaf=cuda=1`, 94.323010 as on
   the CPU), and the series builds clean on gcc 16 and clang 23.
3. **The one `-Wsign-compare` set `0019` introduced is fixed** in the same
   change (`a64multienc.c`, `svq1enc.c`, `vlc.c`: four casts), so the series
   adds no `-Wextra` diagnostic that is not FFmpeg's own `{NULL}` option-table
   convention.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the shims (ADR-0860) | No rebase work | 601 lines and an unfixable warning in every build; the next FFmpeg release conflicts on Vulkan context lines | The cost recurs every release |
| Wait for WP10, which deletes most of these patches anyway | No duplicate work | WP10 lands after the `v1.0.0-rc.3` tag; the series ships the shims until then, and the audit fixes (warnings, README) need a clean base now | WP10 inherits the cleaned series; its own deletions are unaffected (same patch numbers) |
| Renumber the series | Dense numbers | Breaks every reference to a patch number (WP10, ADRs, rebase notes, docs) | Gaps are free |
| Delete only the two files and fix the others by `sed` on the patch text | Faster | Patch hunks carry line counts and context; hand-edited patches fail the replay check | The replay with `git rebase` produces consistent patches |

## Consequences

- **Positive**: 601 patch lines and 80 source lines go; the Vulkan configure
  probe goes; no warning of the series is left in FFmpeg's default set or under
  `-Wextra` (apart from the option-table convention); the next release has
  fewer conflict sites.
- **Negative**: the commit messages of `0012` and `0014` still name Vulkan
  options and the `0003 / 0004 / 0011` numbering (history is not rewritten).
- **Neutral / follow-ups**: ADR-0860 is superseded. `REUSE.toml` loses the
  annotation of `0006` and the `0004` path. WP10 removes the remaining
  `libvmaf*` patches; nothing here changes its list.

## References

- RC4 WP15 brief (FFmpeg audit, 2026-10-07): the maintainer asked for dead code
  in the series, among them the Vulkan shims kept only for hunk context, to be
  removed by rebasing the series without them (paraphrased).
- [Research-2166](../research/2166-ffmpeg-audit-2026-10-07.md): findings S-2,
  B-2, B-3.
- [ADR-0726](0726-drop-vulkan-backend.md), [ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md).
