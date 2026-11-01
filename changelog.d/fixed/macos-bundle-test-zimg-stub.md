- **The macOS bundle script test runs in CI again.** `test_build_script_runs_to_the_end_under_bash32`
  broke when the macOS build script started linking zimg statically from `brew --prefix zimg`,
  because its `brew` stub printed no prefix. CI had not caught it because the RC1 Tester Report job
  never pulled the `bash:3.2` image, so both bash 3.2 tests were skipped. The stub now serves a zimg
  prefix, and the job pulls the image by digest and sets `VMAFX_REQUIRE_BASH32=1`, which turns a
  missing image into a failure instead of a skip.
