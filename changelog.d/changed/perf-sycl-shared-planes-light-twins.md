- **SYCL `psnr_hvs`, `psnr` and `motion_v2` read the frame the device already
  holds (ADR-1369).** A SYCL run uploads each plane of a frame once, and these
  twins now read it there: `psnr_hvs_sycl` no longer converts all three planes
  to float on the host and uploads them a second time, `motion_v2_sycl` no
  longer re-uploads the reference luma, and chroma goes up once per frame for
  every twin that reads it, from a pinned staging buffer. The psnr_hvs kernel
  runs two work-items per 8x8 block in one dispatch for all planes, and psnr
  adds one atomic per work-group instead of one per pixel. At 3840x2160,
  `psnr_hvs` drops from 17.1 to 7.6 ms per frame on an Arc B580 (16 CPU
  threads: 6.6) and from 124 to 60 on a UHD 770, where `psnr` drops from 25.3
  to 12.3; `motion_v2` loses about 1.6 ms of host copy and upload per frame.
  Every score is bit-identical to the previous twins. On the zero-copy VA import path, which imports luma only,
  `psnr_sycl` and `psnr_hvs_sycl` with chroma now fail the frame with an error
  instead of crashing. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#psnr-psnr_hvs-and-motion_v2-share-the-uploaded-frame-adr-1369-2026-09-29).
