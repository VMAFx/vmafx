---
paths:
  - core/src/feature/cuda/ssim_cuda.c
  - core/src/feature/cuda/integer_ssim_cuda.c
invariant: Integer SSIM CPU bits, distinct ssim vs integer_ssim features, and fmad-false flags.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer SSIM CPU bits, options, and terms

- **`integer_ms_ssim_cuda.c` and `integer_ssim_cuda.c` pass
  `channel=0` to `picture_copy()`** per upstream
  d3647c73 prerequisite port. If future upstream commit
  evolves signature further, update these call sites in
  lockstep with upstream-mirror callers (`float_*` series).
  See [../../AGENTS.md §"`picture_copy()` carries `channel`
  parameter"](../../../AGENTS.md).
- **`integer_ssim_cuda` (`ssim_cuda.c`) = CPU bits** (ADR-1424,
  `EXACT_TWINS["ssim"]`). CPU `calc_ssim()` = ONE double accumulator through
  whole frame, raster order. So: `integer_ssim_vert_combine` stores
  double term per pixel (`terms[y * width + x]`), NO warp / block reduction
  of it; host `issim_frame_sum()` adds read-back plane in index order.
  int64 weights: order-free, block reduction stays (`warp_reduce(int64_t)`,
  ADR-1224). Term grouping `((w * a) * b) / den`, fatbin without contraction
  (ADR-1403). Cost: 66 MB read-back + 8.3M sequential adds per 4K frame
  (9.7 ms vs 2.2 ms): `T-CUDA-SSIM-EXACT-THROUGHPUT-2026-10-01` holds
  parallel-exact candidate (integer sums per binade); tune only with
  `test_cuda_ssim_parity` (`==`, 8 cases) green. Guard without device:
  `test_cuda_ssim_exact_contract.py`.
- **`ssim_cuda.c` and `integer_ssim_cuda.c` provide different features — do not
  conflate them** (ADR-0564). `ssim_cuda.c` registers `vmaf_fex_integer_ssim_cuda`,
  provides `"ssim"` (real 9-tap int64 integer SSIM, bit-exact with CPU).
  `integer_ssim_cuda.c` = historical misnomer: registers *different*
  `vmaf_fex_integer_ssim_cuda` symbol, provides `"float_ssim"` (11-tap
  floating-point Gaussian). Both symbols linked, registered in
  `feature_extractor.c`; `feature_extractor_list[]` puts `ssim_cuda.c`'s extractor
  first so `vmaf_get_feature_extractor_by_name("ssim")` resolves correctly.
  **Never swap order** of these two entries. Planned follow-up (post-ADR-0564)
  will rename `integer_ssim_cuda.c` → `float_ssim_cuda.c` to eliminate confusion;
  until then, keep naming mismatch explicit, do not merge two files.

- **`ssim_score.cu::ssim_terms()` = CPU `l * c * s`, operand for operand**
  (`iqa/ssim_tools.c::ssim_variance_scalar` + `iqa/ssim_accumulate_lane.h`):
  double numerators over fp32 denominators, fp32 `s`, double product, each
  rounding intrinsic (`__fmul_rn` / `__fadd_rn` / `__ddiv_rn` ...). Host
  rounds frame means to fp32 (`float_ssim_frame_mean`).
- **`integer_ssim_score` builds with `--fmad=false`** (every fatbin,
  ADR-1403; as HIP `-ffp-contract=off`, ADR-0564 / ADR-1373) and groups term as CPU:
  `w_d * a * b / den` = `((w * a) * b) / den`. Per-pixel terms = CPU bit for
  bit; frame sum order differs (warp / block / host vs CPU row-major) -> no
  bit-exact claim. `test_cuda_kernel_source_contract.py` pins flag + grouping.
