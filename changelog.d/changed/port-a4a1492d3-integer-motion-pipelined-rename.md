## libvmaf: replace integer_motion with pipelined v2 variant (port a4a1492d3)

The CPU `vmaf_fex_integer_motion` extractor now implements the pipelined
row-at-a-time algorithm (formerly `integer_motion_v2`). The dedicated CPU
`vmaf_fex_integer_motion_v2` extractor is removed; GPU backend twins
(`integer_motion_v2_cuda`, `_sycl`, `_hip`, `_metal`) are unchanged.
`motion_avx2`/`motion_avx512` implement the pipeline functions;
`motion_v2_*` SIMD files are kept for GPU build paths.
