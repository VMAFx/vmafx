`psnr_cuda`, `psnr_sycl`, and `psnr_vulkan` now honour the `enable_chroma`
option (default `true`). Previously the option was absent from the GPU
option tables, causing it to be silently ignored and the GPU extractors to
always emit full luma + chroma output even when the caller passed
`enable_chroma=false`. This produced silent JSON divergence from the CPU
reference on non-YUV400 sources. The fix adds the option entry and the
matching `n_planes` clamp in `init()` on all three GPU backends; the
default path is bit-for-bit unchanged. (ADR-0453 / Research-0136)
