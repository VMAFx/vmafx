---
paths:
  - core/src/feature/cuda/integer_adm_cuda.c
  - core/src/feature/cuda/integer_adm_cuda.h
  - core/src/feature/cuda/integer_adm/adm_decouple_inline.cuh
invariant: Integer ADM options, CPU bits, negative rounding terms, tiny frame shifts, and int64 vertical sums.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM options, exact bits, tiny frames, and DWT

- **`integer_adm_cuda.c` must NOT include `feature/adm_options.h`
  directly.** `DEFAULT_ADM_NOISE_WEIGHT`, `DEFAULT_ADM_CSF_SCALE`,
  `DEFAULT_ADM_CSF_DIAG_SCALE`, and full 4-member
  `enum ADM_CSF_MODE` arrive transitively via
  `cuda/integer_adm_cuda.h` → `feature/integer_adm.h`. Direct
  include reintroduces 2-member `enum ADM_CSF_MODE` from
  `adm_options.h`, causes redeclaration error.
- **`adm_cuda` = CPU bits** (ADR-1416, `EXACT_TWINS`). Host arithmetic =
  CPU routines of `feature/integer_adm_kernels.h`, never copy:
  `adm_csf_factors()` (weights; old copy multiplied exponent in
  `float`, CPU in `double` -> 1-3 ulp, 2.1e-7 in scores),
  `adm_csf_den_ctx_init()` / `i4_adm_csf_den_ctx_init()` (border + every
  denominator shift, passed to kernel), `adm_cm_ctx_init()` /
  `i4_adm_cm_ctx_init()` on empty `AdmBuffer` + `adm_cm_result()` /
  `i4_adm_cm_result()` / `adm_csf_den_result()` / `i4_adm_csf_den_result()`
  (scores). No `dwt_quant_step()`, `adm_csf_factors()`, `conclude_adm_*()`
  definition in `integer_adm_cuda.c`. `adm_skip_scale0`: numerator 0,
  denominator `(float)1e-10`, BOTH added to frame sums like
  `integer_adm_scale0()`.
  `adm_csf_den.cu`: one block per row + band (`grid = 1 x rows x 3`,
  128 threads), block reduce, ONE fold per row through
  `adm_csf_den_round_row_total()` (`adm_cm_accumulator.h`, also CPU's
  fold). Never fold per warp / thread / block-of-columns: accumulator
  differs, score differs on low-detail frames (6.6e-7). No logarithm in that
  file: fp32 `__log2f(area) - 20` is off by one for 81 areas right above
  power of two (962x13542: `adm_scale0` 0.860 vs 0.979).
  `adm_cm.cu` device shifts `ceil(log2(w|h))` take integer extents: equal to
  CPU for 1..131071, leave as is.
  Guards: `test_cuda_adm_parity` (`==`, 9 exact cases),
  `test_adm_cm_row_rounding`, `test_cuda_adm_exact_contract.py`,
  `test_adm_cm_row_rounding_contract.py`.
- **CSF weight limits = `adm_csf_fixed_limit()` (ADR-1472), host side
  only.** `adm_cm.cu` narrows `(v * v + round) >> 29|30` to int32 like
  CPU (`adm_cm_accum_round()` / `i4_adm_cm_accum_round()`). It cannot wrap
  because `adm_csf_fixed_scale()` (`feature/adm_csf_fixed_point.h`) keeps
  every weight under excess budget / largest wavelet coefficient of
  scale: 43900 (scale 0 h, v: int16 1/30 magnitude of CSF stage,
  ADR-1917; cube alone allows 46603.4), 65536 (scale 0 d), 279958309, 539893111,
  546406567 (scales 1-3). Old limit 2^30 wrapped in Barten mode: NaN
  numerator (10 px checkerboard), `integer_adm2` 0.587 for 0.784 (1 px).
  Never convert weight on device or in this file; never widen or
  saturate square in kernel (CPU bits). Changed DWT taps / shifts,
  `shift_sq`, `i4_shift_dst` -> constants in header + `BAND_FORMAT` in
  `core/test/test_integer_adm_cm_budget.c` follow. Same for HIP, SYCL,
  Metal twins. Check: `--backend cuda` vs `cpu`, `adm=debug=true:adm_csf_mode=1`,
  both 1080p checkerboards, all 18 outputs equal.
- **`integer_adm_cuda.c` / `float_adm_cuda.c` expose three ADM
  tuning parameters** (`adm_csf_scale`, `adm_csf_diag_scale`,
  `noise_weight`) with same defaults as CPU path (PR #731).
  If upstream Netflix adds or renames these parameters in
  `integer_adm.c` / `float_adm.c`, CUDA twins must update
  in same PR.
- **`integer_adm/adm_cm.cu` (and rest of `integer_adm/`
  subdirectory) carries NVIDIA copyright line** alongside
  Netflix one. Upstream-mirror — keep both headers
  verbatim on rebase.
- **Integer ADM scales 1-3 keep ADR-0155's negative rounding term as
  `INT32_MIN`.** one site in `integer_adm/adm_csf.cu` and both fused sites
  in `integer_adm/adm_cm.cu` deliberately subtract 2^31 before their 32-bit
  right shift to stay Netflix-golden compatible. Do not restore
  `1u << 31` assigned into `int32_t`: it generates NVCC diagnostic `#68-D`.
  Do not widen constant either; that changes ADM output. Focused CUDA 13.4
  builds produced byte-identical fatbins after changing only spelling.
  See [Research-2076](../../../../../docs/research/2076-cuda-adm-signbit-warning.md).

- **Integer ADM DWT row / tap arithmetic in
  `integer_adm/adm_dwt2_rows.h`** (ADR-1374): `adm_dwt2_load_column()`
  reads `adm_dwt2_source_row()` (clamped), `calculate_indices()` reads
  `adm_dwt2_s123_tap()`. `static_assert`s tie kernel instantiation to
  header geometry; tile / rows-per-thread change = header change;
  `test_cuda_adm_dwt2_rows` re-checks every height.

- **`integer_adm/adm_decouple.cu` carries same F3 `__ldg()` fix (ADR-0763).**
  `adm_decouple_kernel` (scale-0, `int16_t`) and `adm_decouple_s123_kernel`
  (scales 1-3, `int32_t`) both extract `const T *__restrict__` read-only band
  pointers before per-pixel body, use `__ldg()` for all reads. Write-back
  band pointers plain non-`const` (no `__ldg()` on stores). Note: file
  currently dead (decouple computation inlined into `adm_csf.cu` /
  `adm_cm.cu` via `adm_decouple_inline.cuh`) — rebase on change to that file
  does NOT affect `adm_decouple.cu`.
  See [ADR-0763](../../../../../docs/adr/0763-cuda-adm-decouple-ldg.md).
- **`integer_adm/adm_cm.cu` `x_sq` reduction requires explicit parentheses around
  `add_shift_sq` before right-shift (r6-cuda-kernel / 2026-06-04).** Expression
  `(int64_t)accum * accum + add_shift_sq >> shift_sq` parsed by C++ as
  `+ (add_shift_sq >> shift_sq)` = `+ 0` because `>>` binds tighter than `+`.
  Correct form = `((int64_t)accum * accum + add_shift_sq) >> shift_sq`, matching
  CPU reference macro `I4_ADM_CM_ACCUM_ROUND` in `integer_adm.c:743` and fused
  kernel at `adm_cm.cu:259`. Defect affected two reduction loops in file
  (lines 373 and 712). On rebase: if either loop modified, verify parenthesisation
  of `x_sq` computation before pushing.
- **`integer_adm/adm_csf.cu` and `integer_adm/adm_cm.cu` carry F3 `__ldg()` fix on
  active path (ADR-0773).** Six inline `__device__` helpers in `adm_cm.cu`
  (`inline_i4_csf_a`, `inline_i4_decouple_r`, `inline_s0_csf_a`, `inline_s0_decouple_r`,
  `inline_i4_csf_r`, `inline_s0_csf_r`) and two kernel templates in `adm_csf.cu`
  (`i4_adm_csf_kernel<>`, `adm_csf_kernel<>`) all extract `const T *__restrict__` band
  pointers from `cuda_*_adm_dwt_band_t` structs before any indexed load. All six per-pixel
  DWT2 band reads use `__ldg()`. When rebasing or modifying these helpers: preserve
  `__restrict__` extraction pattern; do not add writes through these pointers (they are
  read-only inputs). ADR-0773 completes ADR-0756 `adm_decouple` dispatch item.
  See [ADR-0773](../../../../../docs/adr/0773-cuda-adm-decouple-inline-ldg.md).

## Integer ADM tiny frames (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18)

- `init_fex_cuda()` calls `adm_frame_size_check()` first, before any device
  resource. Bound = CPU bound (17x17).
- Shift rounding constant = `adm_half_shift(x)`. Never `1 << (x - 1)`:
  scale-0 h/v cube shift = 0 at frame width 17..32 -> 2^31 on x86.
- Scale-0 CM kernels (`adm_cm_line_kernel`, `adm_cm_aim_line_kernel`):
  `x - 1`, `y - 1` -> `abs()`; `x + 1` -> `min(.., w - 1)`;
  `y + 1` -> `min(.., h - 1)`. Same rule as CPU `adm_cm_thresh()` (ADR-1210).
  Edges enter CM region only for bands <= 14 samples.
- Upstream Netflix form differs; keep fork form on sync (`docs/rebase-notes.md`).
- Guard: `test_cuda_adm_tiny_frames` (scalar CPU ref, 1e-4; rejection test
  needs no device).

## Integer ADM 16-bit vertical DWT sums in int64 (T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18)

- `core/src/feature/cuda/integer_adm/adm_dwt2.cu`, scale-0 fused kernel: vertical accumulator = `DwtVertAccum<T>::type`
  -> int64 for `uint16_t`, int32 for `uint8_t`.
- Low-pass taps 1-3 sum 50582 -> int32 sum overflows (UB) once 3 16-bit
  samples >= 42456. CPU twin: `adm_dwt2_vpass16_tap4()` (int64).
- Normalised value fits int32 -> int64 form = old wrapped result. Scores
  identical; never narrow back to int32 for speed.
- Guard: `test_gpu_adm_bright_16bit_parity` in `test_gpu_adm_tiny_frames.c`
  (parity only; device wrap hides UB itself).
- **Scale-0 decouple reciprocal = `div_lookup`, by arithmetic
  (`T-GPU-ADM-DECOUPLE-FP32-RECIPROCAL-2026-10-03`).** `adm_recip_q30()` in
  `integer_adm/adm_decouple_inline.cuh`: fp32 quotient `q` (off by at most 11),
  exact int32 remainder, `floor(r / |o|)` added with sign of `o`. Equal to
  `div_lookup[o + 32768]` for every `int16` operand; `int32_t(2^30f / float(o))`
  alone is another integer for 343 positive operands and moves restored sample
  above |o| = 16566. Do not restore it, and do not write 32-bit integer division
  there: `adm_cm_line_kernel_8` goes 148 to 228 registers and
  `test_cuda_adm_cm_register_pressure` (ADR-1226) fails. Guards:
  `test_adm_decouple_recip_cuda` (this header compiled for host against
  `adm_decouple_band()`, every `int16` operand, no device) and case
  `test_adm_attenuated_detail_exact` of `test_cuda_adm_parity` on device.
- **Scale 1-3 decouple bounds gain product before narrowing**
  (T-GPU-ADM-S123-GAIN-PRODUCT-NARROWING-2026-10-05). `decouple_r_s123()`:
  `gained = (double)rst_q * adm_enhn_gain_limit`, then
  `(int32_t)(rst_f > 0 ? fmin(gained, t) : fmax(gained, t))`, as CPU's
  `adm_decouple_band_s123()`. |o| reaches 1.45e9 at scale 1, so
  `(int32_t)(...) * gain` narrowed first is undefined conversion (the
  saturating device cvt hid it). Guard: `test_gpu_adm_gain_product_contract.py`.

- **`decouple_angle_flag_s0()` sums in 64 bits (`T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06`, [ADR-2134](../../../../../docs/adr/2134-cuda-adm-cm-aim-register-budget-angle-flag.md)).**
  `integer_adm/adm_decouple_inline.cuh` forms dot product as unsigned sum restored by
  `(int64_t)(int32_t)(sum - 1u) + 1` (one value int32 cannot hold is 2^31, every band at -32768);
  squared magnitudes as unsigned sums widened to int64, as CPU's int64 `adm_angle_flag()` sums are. Do not
  narrow it back to int32. form costs `adm_cm_aim_line_kernel_4` 209 registers (plain int64: 216), which
  is that kernel's own budget in `test_cuda_adm_cm_register_pressure` (every other kernel 208, zero spill);
  RC7 row wins register back. `test_adm_decouple_recip_cuda` holds flag to CPU's at every int16
  corner and expects 0 mismatches. It also holds `decouple_r_s123()`, `get_best15_from32()` and scale 1-3 flag to CPU's,
  so no function of header is unused in host build (CodeQL `cpp/unused-static-function`).
- **Two viewing distances (ADR-2795).** `adm_scale0_transform()` /
  `adm_scale123_transform()` = DWT once per scale; `adm_scale0_weigh()` /
  `adm_scale123_weigh()` = denominator, CSF, CM, AIM per distance, through
  `adm_view_buffer()` (second distance's result slots `adm_*_x`, second
  `RES_BUFFER_SIZE` block of `tmp_res` / `results_host`). Per-distance
  kernels write only `csf_f` / `i4_csf_f` and result slots; never write
  DWT band from weigh stage. Host conclusion takes distance as argument
  (`adm_cm_scale_result()`, `adm_csf_den_scale_result()`), never
  `s->adm_norm_view_dist`. Merge + names = shared `adm_view_dist.c`.
  `test_adm_two_views_exact`, `test_adm_merged_registrations_exact` (`==`).
