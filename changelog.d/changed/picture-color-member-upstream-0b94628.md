- **The pictures guide states what a caller-set colour does now that upstream
  moved `VmafPicture.color` to the end of the struct
  ([ADR-1822](docs/adr/1822-additive-picture-convert.md)).** Source that sets
  `pic.color` does not compile against VMAFx's header, a binary built on
  upstream's header has its colour ignored, and the colour is given through
  `vmaf_set_input_colorimetry()` or the `src_color` argument;
  `test_picture_no_color_member` holds the compile-time refusal.
