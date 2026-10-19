- **FFmpeg builds apply shared fix series `v0.2.0`.** `build-config.env` pins
  `VMAFx/ffmpeg-patches` release `v0.2.0` (commit `d958384a`, still on FFmpeg
  `n9.0.2`), which adds four fixes to the four of `v0.1.0`: VAAPI import of
  10- and 12-bit Vulkan frames, the Vulkan export query's format list, a CPU
  wait before mapping RADV frames to DRM, and Vulkan queue de-phasing. See
  [FFmpeg patch automation](docs/development/ffmpeg-patch-automation.md).
