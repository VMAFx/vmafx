`libvmaf_metal.h` is now installed by `meson install` whenever
`-Denable_metal=enabled` or `-Denable_metal=auto` is set, mirroring the
Vulkan/HIP/SYCL install pattern. The header was previously absent from the
installed tree, causing downstream FFmpeg `--enable-libvmaf-metal` configure
probes to silently fail. `docs/api/gpu.md` Metal and HIP sections updated with
correct symbol names and the complete IOSurface zero-copy import sub-API
(`VmafMetalExternalHandles`, `vmaf_metal_state_init_external`,
`vmaf_metal_picture_import`, `vmaf_metal_wait_compute`,
`vmaf_metal_read_imported_pictures`). ADR-0437.
