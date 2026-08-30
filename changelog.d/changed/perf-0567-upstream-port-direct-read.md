# CLI tools: USE_DIRECT_READ zero-copy frame input path (upstream port)

Port Netflix/vmaf `30a6e2a8d`: add a `USE_DIRECT_READ` compile-time flag to
the VMAF CLI tools (`vmaf`, `vmaf_bench`) that eliminates the intermediate
`video_input_ycbcr` buffer and the per-frame `memcpy` by reading frame data
directly into preallocated `VmafPicture` planes.

When built with `-DUSE_DIRECT_READ`, the `fetch_picture()` path calls the new
`video_input_fetch_into_vmaf_picture()` dispatcher (one allocation + read per
frame) instead of `video_input_fetch_frame()` + `copy_picture_data()` (one
allocation + read + full-frame copy per frame).  The flag is opt-in; the
existing path is retained under `#else` for compatibility.

Y4M input supports the direct path only for formats that require no
colour-space conversion (`convert == y4m_convert_null`); all other Y4M formats
return an error at runtime when `USE_DIRECT_READ` is active.

See ADR-0567.
