### Fixed

- **CIEDE2000 4:2:2 chroma-upsample flag swap (heap OOB read + wrong scores)**
  (`core/src/feature/ciede.c`): `scale_chroma_planes` and
  `scale_chroma_planes_hbd` used `ss_ver` for the horizontal sample index and
  `ss_hor` for the vertical row advance — the two subsampling flags were
  transposed. On YUV422P (half-width, full-height chroma) the horizontal index
  read past the half-width input row (heap OOB) and the vertical advance
  skipped half the input rows, producing wrong `ciede2000` scores. YUV420P
  (both flags set) and YUV444P (early return) were unaffected, so the Netflix
  golden CIEDE2000 test (420P content) never caught it. Fix: horizontal index
  now uses `ss_hor`, vertical advance uses `ss_ver`, matching the CPU
  subsample semantics. Adds regression tests
  `test_ciede_scale_chroma_422_8b` / `_16b` to `core/test/test_ciede.c` with
  non-identical chroma so the upsample pattern is actually validated.
- **cambi init() partial-failure allocation leak**
  (`core/src/feature/cambi.c`): every error return in `init()` (dimension
  validation, picture-pool alloc, contrast/tvi/c-values/histogram/mask buffers,
  heatmap file opening) returned without freeing the buffers, picture pool, or
  feature-name dictionary acquired earlier in the same call — the framework
  never invokes `close()` after a failed `init()`. Fix: route every error path
  through a `fail:` label that calls the null-tolerant `close_cambi()`
  (`fex->priv` is zero-initialised, so unallocated pointers are NULL).
  `close_cambi()`'s heatmap `fclose` loop is now NULL-guarded for the
  partial-open case. No scoring path changed.
- **integer SSIM 16-bpc `samplemax²` signed-integer overflow (corrupt c1/c2;
  CPU↔GPU divergence)** (`core/src/feature/integer_ssim.c`):
  `ssim_reduce_row_range` computed `c1/c2 = samplemax * samplemax * K * w_d²`
  with `int samplemax = (1<<depth)-1`. For 16-bpc input, `samplemax = 65535`
  and `65535 * 65535 = 4 294 836 225 > INT_MAX`, so the multiply overflowed
  `int` (signed-overflow UB, wrapping to −131071) and the SSIM stability
  constants went negative — corrupting every 16-bpc SSIM term and diverging
  from the CUDA / HIP / SYCL twins, which already widen to `int64_t` / `double`.
  8/10/12-bpc (`samplemax² ≤ 16 769 025`) fit in `int` and are bit-unchanged.
  Fix: hoist `const double sm = (double)samplemax;` and compute
  `c1/c2 = sm * sm * K * w_d²`, matching the GPU twins. Adds load-bearing
  regression test `test_ssim_16bit_distorted_in_range` to
  `core/test/test_ssim_coverage.c` (anti-correlated 16-bpc content scores
  0.999992 with the fix vs 1.185 — out of `[0,1]` — with the overflow).
  (round-2 bug-hunt finding R2-1.)
