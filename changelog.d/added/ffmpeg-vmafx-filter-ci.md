- **CI builds and checks the `vmafx` FFmpeg filter (#2475).** The required
  check `FFmpeg vmafx Filter` builds FFmpeg with `--enable-libvmafx` and
  compares the filter's report with the `vmaf` CLI (`parity`, `windows`,
  `contract`, `provenance`) on every pull request that touches the core, the
  patches or the workflows
  ([Checking a vmafx build](docs/usage/ffmpeg.md#checking-a-vmafx-build)).
