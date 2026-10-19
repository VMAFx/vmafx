- Windows builds: the VMAFx model-spec test escapes the colon of a drive-letter
  model directory before putting it in a spec, and the FFmpeg `vmafx` filter's
  generated `vf_vmafx_options.h` uses the guard name `AVFILTER_VMAFX_OPTIONS_H`
  that FFmpeg's `fate-source` check requires
  ([FFmpeg patch automation](docs/development/ffmpeg-patch-automation.md)).
  `scripts/ci/test_ffmpeg_patch_stack.py` refuses a header the series adds with
  any other guard.
