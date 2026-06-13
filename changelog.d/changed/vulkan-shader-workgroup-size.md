### Vulkan compute shader workgroup size fix (VK-1 + VK-2)

Raised `local_size_x` from 1 to 32 in `ssimulacra2_blur.comp` (VK-1) and in
`cambi_mask_dp.comp` PASS 0/1 (VK-2), fixing a performance defect flagged in
`perf-audit-vulkan-sycl-2026-05-16`. Each shader previously ran one thread per
workgroup, leaving 31 of 32 lanes idle on NVIDIA (warp = 32) and 63 of 64 idle
on AMD (wavefront = 64).

**VK-1 — `ssimulacra2_blur.comp`**: The IIR Gaussian blur runs 6 scales × 3 planes
× 2 passes = 36 dispatches per SSIMULACRA2 frame. With `local_size_x = 32` each
dispatch now saturates one full warp/wavefront. Host dispatch changed from
`(1, H, 1)` / `(1, W, 1)` to `(ceil(H/32), 1, 1)` / `(ceil(W/32), 1, 1)`.
Estimated 30-50% reduction in SSIMULACRA2 frame time on RTX 4090.

**VK-2 — `cambi_mask_dp.comp`**: The row and column prefix-sum passes (PASS 0/1)
now dispatch `ceil(H/32)` and `ceil(W/32)` workgroups respectively. At 1080p this
reduces PASS 0 from 1080 single-thread dispatches to 34 full-warp dispatches.
Estimated 5-15% reduction in CAMBI Vulkan frame time.

Bit-exactness (ADR-0214, places=4): the IIR state in the blur shader is private to
each invocation; no cross-lane communication is needed. The prefix-sum in cambi is
integer accumulation within one invocation. Neither change reorders any arithmetic
— parallelism comes only from running independent rows/columns concurrently.

Benchmark: host RTX 4090 contended by CHUG run at time of change; before/after
numbers deferred to the next available benchmark window.
