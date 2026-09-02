- **ciede init() leak:** when `vmaf_picture_alloc` fails during `ciede` feature
  init, the aligned `tmp[]` scratch buffers (and a successfully-allocated `ref`
  picture when `dist` allocation fails) leaked, because the extractor framework
  does not call `close()` on init failure. Both failure paths now free every
  prior allocation. Error-path only; scoring is unchanged.
