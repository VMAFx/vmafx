## HIP ADM: AdmBufferHip passed by pointer (ADR-0759)

`AdmBufferHip` (~272 bytes) was previously passed by value in four HIP `__global__`
kernel signatures in `adm_csf.hip` and `adm_cm.hip`. Each kernel launch marshalled
the full struct through the per-launch argument buffer.

The struct now passes as `const AdmBufferHip * __restrict__ buf_ptr`. A device-side
copy is allocated once at init time and stable for the extractor's lifetime.

Impact: reduced per-launch argument-buffer overhead on all four ADM kernels
(`adm_csf_kernel_1_4`, `i4_adm_csf_kernel_1_4`, `i4_adm_cm_line_kernel`,
`adm_cm_line_kernel_8`) on every AMD GPU target. Numerically transparent.

Mirrors the CUDA F3 fix (PR #93 / PR #96). ADR-0759.
