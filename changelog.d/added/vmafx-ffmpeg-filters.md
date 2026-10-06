- **FFmpeg `vmafx` filter (RC4 WP9, ADR-2125).** Patch `0021` adds the `vmafx`
  filter, the FFmpeg filter of the VMAFx API (`./configure --enable-libvmafx`,
  `libvmafx >= 1.0.0`; it can be built next to `--enable-libvmaf`). It scores
  one or several named models plus extra features in one pass, pools over
  `n_stats` windows, attaches per-frame scores as `lavfi.vmafx.<model>`
  metadata, embeds the provenance record, and follows its frames to their
  device: CUDA frames are imported without a copy on CUDA, VAAPI frames mapped
  to DRM PRIME on SYCL (Intel) or HIP (AMD), the 4:2:2 / 4:4:4 semi-planar,
  packed and MSB-aligned layouts are taken as well, software frames score on
  the CPU, and a frame it cannot import fails the graph naming the backend and
  the extractor instead of passing unscored. `vmafx_tune` (patch `0021`) and
  `vmafx_pre` (patch `0022`) are the VMAFx names of `libvmaf_tune` and
  `vmaf_pre` and give the same results (`vmafx_tune` defaults to the library's
  default model); `-vmafx-profile` (patch `0023`) is the VMAFx name of
  `-vmaf-profile`. The `libvmaf*` filters stay until their retirement. See
  [using VMAF with FFmpeg](docs/usage/ffmpeg.md#the-vmafx-filter).
- **Hardware decoding for loopback decoders in FFmpeg (RC4 WP9, ADR-2125).**
  Patch `0024` lets `-hwaccel`, `-hwaccel_device` and `-hwaccel_output_format`
  precede `-dec`, so an encoder's output decodes on the GPU and reaches the
  `vmafx` filter as device frames: encode, decode and score in one command
  (#2138). The patch is carried by the fork only. See
  [encode and score in one command](docs/usage/ffmpeg.md#encode-and-score-in-one-command).
- **`vmafx` refuses a frame pool it would exhaust (RC4 WP9, ADR-2125).** A
  fixed-size hardware frame pool (VAAPI, QSV, D3D11, D3D12, DXVA2) smaller than
  the frames the filter holds is refused before the first frame with the size
  it needs and the option that sets it, instead of failing mid-stream after a
  partial score. See [frame pools](docs/usage/ffmpeg.md#frame-pools).
