- `libvmaf_metal` ffmpeg filter — dedicated VideoToolbox hwdec zero-copy
  import path mirroring `libvmaf_vulkan`. Consumes
  `AV_PIX_FMT_VIDEOTOOLBOX` frames, pulls the IOSurface backing each
  `CVPixelBufferRef` via `CVPixelBufferGetIOSurface`, and routes it
  through new `vmaf_metal_picture_import` /
  `vmaf_metal_read_imported_pictures` C-API entry points
  (`core/include/libvmaf/libvmaf_metal.h`). The libvmaf-side
  runtime locks the IOSurface read-only and memcpys each plane
  into a shared-storage `VmafPicture` (Apple Silicon unified-memory
  cost is equivalent to a Shared MTLBuffer copy). On hosts without
  an Apple-Family-7 MTLDevice the import path returns `-ENODEV` and
  the filter surfaces `AVERROR(ENODEV)` at `config_props` time; see
  [ADR-0423](docs/adr/0423-metal-iosurface-import-scaffold.md).
  The FFmpeg patch passes `handles.device = 0` so libvmaf falls
  back to `MTLCreateSystemDefaultDevice` until upstream ships
  `AVMetalDeviceContext`.
  Companion to `--enable-libvmaf-metal` from
  `ffmpeg-patches/0012-libvmaf-wire-metal-backend-selector.patch`
  (software AVFrame input + Metal compute on the regular `libvmaf`
  filter); together the two ship the full Metal integration surface
  on FFmpeg. New ffmpeg patch:
  `ffmpeg-patches/0013-libvmaf-add-libvmaf-metal-filter.patch`.
