- **The SYCL timing summary follows the log level.** Every SYCL flush printed
  `[vmaf-sycl] timing: ...` straight to stderr, so FFmpeg's `libvmaf_sycl`
  filter showed it even at `-loglevel error`. It now goes through libvmaf's
  logger at the info level: FFmpeg prints it from `-loglevel info` up, and the
  `vmaf` CLI still prints it by default, now with the `libvmaf INFO` prefix
  every other libvmaf message carries. `vmaf_sycl_profiling_print()`
  (`VMAF_SYCL_PROFILE=1`) keeps writing to stderr.
