- `float_adm_dwt2_avx2` and `float_adm_dwt2_neon` now mirror the
  AVX-512 sibling's NULL guard on the per-call `aligned_malloc` for
  the per-row `tmplo` / `tmphi` scratch buffers. If either allocation
  fails the function now releases the survivor (if any) and returns
  cleanly instead of NULL-derefing the subsequent `_mm256_storeu_ps`
  / `vst1q_f32` store at the first vertical-pass column. The scalar
  reference (`adm_dwt2_s` in `core/src/feature/adm_tools.c`) returns
  `-ENOMEM` on the same condition; matching here pre-emptively
  hardens the SIMD entry points which are compiled today but not yet
  wired into `compute_adm` (ADR-0873 follow-up). No bit-exact change
  on the success path. Also restores the readable doc-comment block
  in `ssimulacra2_host_neon.c` that a prior sed pass scrambled with
  literal `#pragma` text inside the C comment.
