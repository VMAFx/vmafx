**ffmpeg-patches series.txt**: patches 0004 (Vulkan backend selector) and 0006
(`libvmaf_vulkan` filter) were present under `ffmpeg-patches/` but missing from
`series.txt`. The `ffmpeg-vulkan` CI job depends on both patches being applied in
series; the omission meant Vulkan symbols (`vmaf_vulkan_state_init`,
`vmaf_vulkan_state_init_external`) were not wired into the FFmpeg build when
applying the full stack from `series.txt`.
