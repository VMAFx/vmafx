- `motion_sycl` now honours `motion_add_uv=true` (alias `mau`): the SYCL
  extractor runs the 5-tap Gaussian blur and SAD kernel on the U and V chroma
  planes in addition to luma, then sums the three per-plane normalized SADs
  on the host — matching the semantics of `float_motion(motion_add_uv=true)`.
  Previously the option was silently ignored, causing CHUG / K150K sweeps
  that set `mau=true` to receive the Y-only score without any error.
  The CUDA, Vulkan, HIP, and Metal backends now surface the option but reject
  it with `-ENOTSUP` and a `WARNING` log, directing callers to `motion_sycl`
  until per-backend kernel ports land. All `motion_five_frame_window`
  rejection messages have been upgraded from `ERROR` / silent to `WARNING`
  for consistency. ADR-0989.
