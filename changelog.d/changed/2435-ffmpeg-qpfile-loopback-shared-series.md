- **FFmpeg `-qpfile` and GPU loopback decoding now come from the shared fix series.**
  The fork's own `0001-codec-qpfile-unified.patch` and `0006-fftools-loopback-decoder-hwaccel.patch`
  are dropped, together with `ffmpeg-patches/src/ffmpeg_dec_hwaccel.c`: the shared FFmpeg fix series
  (`VMAFx/ffmpeg-patches`, patches `0043` and `0044`) now carries both changes for every consumer.
  The remaining VMAFx patches are renumbered `0001` to `0004` without gaps (issue #2435).
  The `-qpfile` codec option and a loopback decoder's `-hwaccel`, `-hwaccel_device` and
  `-hwaccel_output_format` options keep their names, so
  `vmaf-tune` and existing command lines work unchanged.
