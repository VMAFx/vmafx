- **The direct y4m reader reads a stream shallower than the picture left-shifted to the picture's depth.**
  `video_input_fetch_into_vmaf_picture()` copied an 8-bit y4m frame into a 10-bit picture as raw
  bytes, so half the picture held the frame and the next frame started at the wrong offset (port of
  Netflix/vmaf PR 1682). A stream deeper than the picture, or an 8-bit picture, is now refused. The
  `vmaf` CLI does not use this reader, so its scores do not change.
