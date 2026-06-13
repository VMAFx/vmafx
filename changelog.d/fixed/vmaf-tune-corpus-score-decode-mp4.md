- **`vmaf-tune corpus` score path now decodes container → raw YUV
  before invoking the libvmaf CLI.** Phase A bug-fix ([ADR-0237](docs/adr/0237-quality-aware-encode-automation.md)):
  the encoder adapter writes mp4 (libx264) but `libvmaf`'s CLI
  consumes only raw YUV/Y4M on `--distorted`. Without the decode-back
  step every corpus row landed with `vmaf_score=NaN` /
  `exit_status=234`. `tools/vmaf-tune/src/vmaftune/score.py` now
  transparently shells out to `ffmpeg -f rawvideo -pix_fmt <pix_fmt>`
  in the score scratch workdir when the distorted suffix is not
  `.yuv` / `.y4m`; the temp YUV is cleaned up with the workdir.
  Smoke-verified locally on `BigBuckBunny_25fps.yuv` (1920×1080, 25fps,
  150 frames): `crf=23 → vmaf=96.30`, `crf=33 → vmaf=81.86` (sane,
  was both `NaN` pre-fix). 16/16 unit tests pass (3 new regression
  tests for the decode-back path: mp4 distorted, raw-yuv distorted,
  decode-failure NaN propagation).
