- **Float extractors report their errors through the log (ADR-1906).** The
  allocation and stride errors of the float ADM, SSIM, MS-SSIM, motion and VIF
  code (`error: ...` lines) went to standard output, where they mixed with
  anything a program writes there and ignored the log level. They are now
  `ERROR` log lines: on stderr at the configured level for `libvmaf.h` and
  the CLI, and in the context's log callback for the VMAFx API.
