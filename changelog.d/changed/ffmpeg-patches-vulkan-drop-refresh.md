## ffmpeg-patches: post-Vulkan-drop series refresh (ADR-0726)

Removed orphaned `0004-libvmaf-wire-vulkan-backend-selector.patch` and
`0006-libvmaf-add-libvmaf-vulkan-filter.patch` from disk (series.txt had
already dropped them in PR #47). Regenerated all 13 remaining patches from
a clean n8.1.1 baseline to eliminate stale Vulkan context lines that
prevented `git am` from applying the series. Full replay (0001–0003, 0005,
0007–0015) verified clean against pristine n8.1.1 with zero conflicts.
