---
paths:
  - core/src/feature/sycl/integer_adm_sycl.cpp
  - core/test/test_sycl_adm_parity.c
invariant: Integer ADM tiny frames and linkage; Scale 0 = CPU int16 semantics; scales 1-3 >> 32 rounding term.
---
<!-- markdownlint-disable MD013 MD060 -->
# Integer ADM tiny frames and linkage (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18)

- `init_fex_sycl()` calls `adm_frame_size_check()` first, before any device
  resource. Bound = CPU bound (17x17).
- `integer_adm_sycl.cpp` internals live in one anonymous namespace; only
  `extern "C"` extractor struct has external linkage. No C-style `static` at
  file scope, no linkage NOLINT band.
- Scale 0 = CPU int16 semantics (T-SYCL-ADM-INT16-SEMANTICS-2026-09-18). CPU
  stores bands as `int16_t`; kernels compute wider, so wrap explicitly with
  `adm_i16()` (mod 2^16, compiler-independent) wherever CPU narrows:
  `csf_a`, `csf_f`. Never widen these. Diagonal `csf_a` rounds with 65535,
  not `1 << 16`. NOT narrowed since ADR-1402: CM 1/15 centre tap
  (`adm_dev_csf_centre()`, int32 like CPU `adm_cm_thresh()`); scale-0 CM
  excess = `adm_dev_cm_excess_s0()`, int64, capped at INT32_MAX like CPU
  `adm_cm_excess_s0()`. Never put `adm_i16()` back on centre term.
- Scales 1-3 `>> 32` rounding term = `I4_FLT_ROUND` = -2^31: CPU's
  wrapped `(int32_t)(1u << 31)` (Netflix#955, ADR-0155), same as CUDA / HIP.
  Netflix fixes #955 -> change CPU and this constant together.
- Host finalisation = CPU's float arithmetic (ADR-1362) -> every ADM
  output bit-exact. old double finaliser (`conclude_adm_cm` /
  `conclude_adm_csf_den`, ~1e-7 residual) is gone; do not bring it back.
- Guard: `test_sycl_adm_tiny_frames` (tiny frames, full-range noise,
  isolated patches; bit-exact arm).

- **`integer_adm_sycl.cpp` / `float_adm_sycl.cpp` expose three ADM
  tuning parameters** (`adm_csf_scale`, `adm_csf_diag_scale`,
  `noise_weight`) with same defaults as CPU path (PR #731).
  If upstream Netflix adds or renames these parameters in
  `integer_adm.c` / `float_adm.c`, SYCL twins must update
  in same PR.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_adm_sycl.cpp` | `integer_adm.c` | `test_sycl_adm_parity.c` | ADR-0884 (round 2) |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_adm_sycl.cpp` | `test_sycl_adm_parity.c` | [ADR-0884](../../../../../docs/adr/0884-sycl-kernel-coverage-round2.md) |

- **`dwt_quant_step()` here = copy of CPU statement (ADR-1475).** Exponent
  `params->k * temp * temp` in named `float`, then
  `std::pow(10.0, (double)exponent)`. No `(double)` on product operand:
  weights leave CPU values by 1..3 ulp, `adm.sycl` exact cell goes red. CPU
  statement changes (`integer_adm_kernels.h`) -> this copy, same PR. Guard:
  `core/test/test_integer_adm_quant_step_contract.py`.

- **Two viewing distances (ADR-2795).** DWT once per scale
  (`enqueue_adm_dwt()`); `enqueue_adm_reductions()` per distance, with
  that distance's `i_rfactor[view]` and accumulator block
  `d_accum + view * ADM_ACCUM_SLOTS`. Queue in order: per-distance kernels
  write only `csf_f`, `csf_f_aim` and own block, never DWT band. `rfactor`,
  `i_rfactor`, `csf_normalization_shift` indexed `[view]`; host conclusion
  (`adm_scale_cpu()`, `adm_terms()`, `collect_view()`) takes distance as
  argument. Memset + readback size = `ADM_ACCUM_BYTES * adm_sycl_views()`.
  Debug scores = first distance only. Merge + names = shared
  `adm_view_dist.c`. `test_adm_two_views_exact` (8 + 10 bit, model
  options), `test_adm_merged_registrations_exact` (`==`).
