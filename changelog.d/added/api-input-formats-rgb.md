- **AYUV, UYVY and V210 host input, RGB with a stated matrix (RC4 WP13, ADR-2145, ADR-2146, ABI 0.1.11).**
  `vmafx_frame_import()` takes `VMAFX_PIXEL_FORMAT_AYUV`, `UYVY422` and `V210` next to the other
  semi-planar and packed layouts, and `RGB`, `RGBA` and `BGRA` (8 to 16 bits) converted to Y'CbCr in
  64-bit integers (BT.601, BT.709, BT.2020 NCL) with a matrix, range and transfer the caller states in
  the new `rgb_matrix`, `rgb_range`, `rgb_transfer` and `rgb_out_range` fields; without them the import
  is refused naming the field. The CPU, CUDA and HIP conversions return the same integers. One table
  (`[[pixel_formats]]` of `core/api/vmafx.toml`) generates the import layouts, the FFmpeg and GStreamer
  format lists (`core/api/generated/`) and [the pixel format page](docs/usage/pixel-formats.md). See
  [Importing a frame](docs/api/vmafx/index.md#importing-a-frame).
