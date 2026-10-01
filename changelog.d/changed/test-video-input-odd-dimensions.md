- **Regression test for reading frames with an odd width or height.** The raw
  and y4m readers consume a whole frame when a dimension is odd, where upstream
  Netflix/vmaf loses framing and crashes (Netflix/vmaf PR #1604). The behaviour
  is unchanged, and `test_video_input_odd_dims` now holds it in place for both
  reader entry points ([CLI](docs/usage/cli.md)).
