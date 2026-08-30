- **integer_ssim AVX2 16-bit sign overflow** (`core/src/feature/x86/integer_ssim_avx2.c`):
  reordered 16-bit SIMD moment accumulation from `w*(s*s)` to `(w*s)*s` so neither
  `_mm256_mul_epi32` operand ever reaches 2^31, fixing silent wrong-sign SSIM for
  pixels >= 46341 (16-bit content). Adds regression test `test_integer_ssim_avx2_16bpc_bright`.
- **bpc validation operator** (`core/src/libvmaf.c`): changed `&&` to `||` in
  `validate_pic_params` so ref/dist bit-depth mismatches are rejected on frame 0, matching
  the sibling w/h/pix_fmt guards. Adds test `test_validate_pic_params_bpc` (5 sub-cases).
- **vmafx-server DoS cap** (`cmd/vmafx-server/`): added `ScoreLimiter`
  (`golang.org/x/sync/semaphore.Weighted`) shared across HTTP `/v1/score` and gRPC `Score`;
  excess callers receive HTTP 429 or gRPC `ResourceExhausted`; default cap is
  `runtime.NumCPU()`; configurable via `--max-concurrent-scores`.
- **CUDA PREV_REF UAF + dist translate swallow** (`core/src/libvmaf.c`): replaced bare
  struct copy in Phase 2 PREV_REF submit loop with `vmaf_picture_ref`; propagated the
  previously `(void)`-discarded error from `dist` translate, preventing silent partial-init
  of `dist_device`.
