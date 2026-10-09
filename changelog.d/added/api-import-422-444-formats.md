- **4:2:2 and 4:4:4 frames import on a device (RC4, ADR-2133, ABI 0.1.9).**
  `vmafx_frame_import()` takes semi-planar `VMAFX_PIXEL_FORMAT_NV16`, `P210`,
  `P216`, `NV24`, `P410`, `P416`, the packed layouts `Y210`, `Y212`,
  `YUYV422`, `Y410` (Intel XV30), `XV36`, `VUYX`, and `YUV444P_MSB` (NVDEC's
  10- and 12-bit 4:4:4 words with the sample in the top bits), next to NV12 /
  P010 / P016. A CUDA, HIP or CPU device converts them to planar by a gather,
  a shift and a mask and nothing else, so an imported frame scores bit for bit
  as the same frame created on the host. See
  [Importing a frame](docs/api/vmafx/index.md#importing-a-frame) for the table
  of layouts and what NVDEC and the Intel and AMD VAAPI decoders emit.
