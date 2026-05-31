<!-- markdownlint-disable MD060 -->
# ADR-0860 — Re-include Vulkan FFmpeg patches as no-op shims for chain coherence

| Status   | Date       | Supersedes | Superseded by |
|----------|------------|------------|---------------|
| Accepted | 2026-05-30 | —          | —             |

## Context

[ADR-0726](0726-drop-vulkan-backend.md) removed the Vulkan backend from the
fork and listed `ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch`
and `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch` as files to be
removed in the same PR. The patch files themselves were retained in tree (only
removed from `series.txt`) — the assumption was that downstream patches
(0005, 0007–0015) could continue to apply against `release/8.1` baseline
without 0004 / 0006 in the chain.

That assumption was wrong. Patches `0005`, `0008`, `0010`–`0015` were
generated against the cumulative tree-state with `0004` applied. Their
hunk *context lines* reference Vulkan-conditional blocks (`#if
CONFIG_LIBVMAF_VULKAN`, `enabled libvmaf && check_pkg_config
libvmaf_vulkan …`, `extern const FFFilter ff_vf_libvmaf_vulkan`). With
`0004` absent, every downstream patch's `git am --3way` call fails with
`sha1 information is lacking or useless`, blocking the `FFmpeg — SYCL
(Build Only)` integration leg.

## Decision

Re-include `0004-libvmaf-wire-vulkan-backend-selector.patch` and
`0006-libvmaf-add-libvmaf-vulkan-filter.patch` in `ffmpeg-patches/series.txt`
as **no-op compatibility shims**.

Without `libvmaf_vulkan.h` (deleted per ADR-0726), the `check_pkg_config
libvmaf_vulkan` probe in patch 0004 fails silently and `CONFIG_LIBVMAF_VULKAN`
evaluates to `0` at FFmpeg configure time. All Vulkan-conditional code in
`vf_libvmaf.c` is guarded by `#if CONFIG_LIBVMAF_VULKAN`, so it compiles out.
Patch 0006 adds the `vf_libvmaf_vulkan` source file gated on
`CONFIG_LIBVMAF_VULKAN_FILTER`, which is similarly zero.

The Vulkan **runtime** stays gone (per ADR-0726). The patches contribute
zero linked code; their only effect is preserving the patch chain's
context-line continuity so downstream patches `git am --3way` cleanly.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Regenerate 0005, 0007–0015 against post-0003 tree (full surgery) | Cleanest — series ships without Vulkan-referencing context lines | Several hours of patch-by-patch context fix-ups; high risk of introducing semantic diffs; each patch's downstream order matters | Effort vastly disproportionate to the gain; no behavioural change |
| Disable the SYCL CI leg until the patch series is rebased | Unblocks merge train immediately | "Deactivates a check rather than fixes the root cause" — explicitly forbidden by user direction | Not an acceptable path per session ground rules |
| Replace `git am --3way` with `patch -p1 --forward -F 3` | Tolerates context drift via fuzz | Patch 0010 hits "Reversed (or previously applied) patch detected" because upstream FFmpeg 8.1 already ships `--enable-libvmaf-cuda`; `--forward` still skips. Not a complete fix. | Doesn't actually resolve the chain |
| Re-include 0004 + 0006 as no-op shims (chosen) | One-line `series.txt` change; preserves chain coherence; zero runtime effect | Mild documentation overhead (this ADR) | Smallest viable fix; honours ADR-0726 spirit (Vulkan runtime is gone) |

## Consequences

- **Positive**: SYCL CI leg unblocks; downstream patches `0005`, `0007`–`0015`
  apply cleanly without surgery.
- **Negative**: Two `series.txt` entries reference Vulkan, which a future
  reader might initially read as "Vulkan is back". This ADR is the
  cross-link that clarifies the intent.
- **Neutral / follow-ups**: A future patch-series rebase against a newer
  FFmpeg baseline (post-8.1) is the right moment to drop the shims and
  regenerate the chain in one go. Not urgent.

## References

- ADR-0726 (drop Vulkan backend) — the decision this ADR supplements
- ADR-0186 (Vulkan image-import contract) — historical context on Vulkan
  patch design
- `ffmpeg-patches/series.txt` — the file modified
- `ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch`
- `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`
- Reproducer: clean `release/8.1` checkout, apply `0001-0003`, then
  attempt `git am --3way 0005-libvmaf-add-libvmaf-sycl-filter.patch` →
  `sha1 information is lacking or useless (configure)`. Add `0004` to
  the chain and the same patch applies cleanly.
