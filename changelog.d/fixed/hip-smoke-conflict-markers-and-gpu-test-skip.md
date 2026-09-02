fix(test): resolve Git conflict markers in test_hip_smoke.c; GPU tests skip gracefully when no device

`core/test/test_hip_smoke.c` had two unresolved Git conflict markers at
the float_ansnr_hip function and test-list entry, left over from the
post-merge-train sweep commit (24bb5daf89). Both markers removed: HEAD
side retained, which correctly omits the float_ansnr_hip test (dropped
by ADR-0720 / PR #38).

`core/test/test_gpu_picture_pool.c` and `core/test/test_cuda_pic_preallocation.c`
previously hard-failed (SIGSEGV via mu_assert NULL-deref) when no CUDA
device was present. Both tests now check the return value of
`vmaf_cuda_state_init` and emit `[skip: no CUDA device]` + return NULL
to pass cleanly on CPU-only CI runners.
