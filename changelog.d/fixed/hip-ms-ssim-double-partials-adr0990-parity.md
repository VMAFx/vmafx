**HIP ms_ssim_vert_lcs: promote to double precision, add enable_db/clip_db (ADR-1071)**

The HIP `ms_ssim_vert_lcs` kernel now computes per-pixel L/C/S and performs
warp/block reductions in `double` precision, matching the CUDA fix applied in
ADR-0990. Previously the kernel used `float c1, c2, c3` parameters and wrote
`float` per-block partials, producing ~0.004 drift per scale relative to the
CPU reference (`ssim_tools.c` uses `2.0 *` double literals for the L/C/S
numerators).

Host-side: `MsSsimStateHip.c1/c2/c3` promoted to `double`; device and
pinned-host partial buffers resized to `sizeof(double)`; `hipMemcpyAsync`
DtoH copies updated accordingly.

`enable_db` and `clip_db` options are now wired into the HIP extractor's
`options[]` array and `collect_fex_hip()`, matching the CPU (`float_ms_ssim`)
and CUDA (`float_ms_ssim_cuda`) behaviour. Previously these options were silently
ignored on HIP, returning linear MS-SSIM regardless of the caller's request.
