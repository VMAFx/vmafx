- **The patched FFmpeg is built with MSVC against `vmaf.lib` on every change
  (Netflix/vmaf `3e1385bed`).** The required check `FFmpeg Windows MSVC`
  builds the configured FFmpeg release with the whole `ffmpeg-patches/` series
  through `configure --toolchain=msvc` against a static MSVC install, and
  requires the libvmaf filter's scores to equal the CLI's and no compiler
  warning on the lines the series writes. The `vmaf_pre` and
  `libvmaf_tune` filters pass their pixel-format lists through FFmpeg's typed
  helper, which `cl.exe` had reported as an incompatible-type warning.
  `ffmpeg-patches/test/build-and-run.sh` takes `FFMPEG_TOOLCHAIN=msvc`,
  `FFMPEG_JOBS`, `SMOKE_FATE` and `VMAF_SCORE_CHECK`
  ([FFmpeg with MSVC](docs/getting-started/building-on-windows.md#ffmpeg-with-msvc)).
