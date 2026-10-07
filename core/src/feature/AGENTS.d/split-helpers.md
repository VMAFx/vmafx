---
paths:
  - core/src/feature/feature_extractor.cpp
invariant: Helper functions split to meet LOC bounds must preserve expressions without truncation.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Split Helpers Expression Integrity and Inlining

## Split helpers must not split an expression (HISS-04, ADR-1253)

When function here is split to fit 60-LOC bound, helper is `static` in same
TU, never reached through function pointer, never in another TU. Arithmetic
moves whole: each statement lives on one side of helper boundary. Reason is
FMA contraction: `a * b + c` inside helper contracts same way it did inline,
but `t = a * b;` in caller plus `t + c` in helper does not, and that is 1 ULP
that ssimulacra2 pooling amplifies into visible score delta (ADR-1205).
Reductions keep their order: move whole accumulation loop, never partial sums.

Functions carrying ADR-0141 §2 bit-exactness carve-outs with paired SIMD ports
stay unsplit — `adm_dwt2_s`, `calc_ssim`, `niqe_extract_aggd`,
`create_recursive_gaussian`, `picture_to_linear_rgb`. `compute_adm` left that
list (ADR-1142, RC3 debt): it holds no kernel arithmetic, only allocation,
call order and four double accumulators, and is split along those lines —
`adm_frame_alloc()` / `adm_frame_free()` (buffers, upstream's stdout
messages), `adm_scale_dwt2()`, `adm_scale_sums()` (decouple, den, csf, cm,
csf, cm: upstream's order, do not reorder), `adm_accumulate_scales()` (float
per-scale sums added into doubles: num, den, then aim_den before aim_num).
`compute_adm()` keeps name + signature. Upstream hunk in `compute_adm` ->
port into owning helper. `ADM_OPT_DEBUG_DUMP` blocks gone (called
`write_image` / `PRINTF`, defined nowhere: could not compile). For
`brisque_fit_aggd` in
`brisque_math.h` (pure scalar, no SIMD twins), inner loop was extracted to
`static brisque_aggd_accumulate` to satisfy HISS-04 (60 LOC max), keeping both
accumulation loops intact and statements unsplit.
