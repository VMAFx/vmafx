---
paths:
  - core/src/feature/sycl/integer_cambi_sycl.cpp
  - core/test/test_sycl_cambi_parity.c
invariant: integer_cambi_sycl.cpp — fully device-resident, graph-registered; options_cambi_sycl sync.
area: ssim-cambi-and-metrics
---
<!-- markdownlint-disable MD013 MD060 -->
# CAMBI banding extractor and kernels

- **`integer_cambi_sycl.cpp` — fully device-resident, graph-registered**
  ([ADR-1357](../../../../../docs/adr/1357-sycl-cambi-device-resident.md),
  supersedes the ADR-0415 / ADR-0489 host residual). Reads distorted luma
  from shared frame (`enqueue_fn` `shared_dis`), no own upload. Whole frame
  = one `enqueue_cambi_work`: reset → validate → preprocess → tiled mask →
  per scale {decimate, filter H, filter V + level map Q, `launch_row_masks`,
  `launch_c_values` (+ radix pass 0 + per-group sum), `launch_topk_pooling`}.
  `post_fn` = only D2H (88-byte `CambiSyclResults`); `collect()` = only host
  arithmetic (`vmaf_cambi_weight_scores_per_scale`). Load-bearing:
  - every kernel argument init-time state: graph recording replays
    `enqueue_fn` for both slots; no host decision per frame, no
    `memset`/`fill` inside `enqueue_fn` (reset is a kernel);
  - c-values multiply by `vmaf_cambi_reciprocal_lut()` table, never
    `1.0f / i` (42 entries differ by 1 ulp);
  - top-K sum exact: 128-bit fixed point, units 2^-24 (all non-zero
    c-values in [0.5, 2^14)); no fp64, no float accumulation;
  - `check_window_fits_lut` = `cambi.c::setup_contrast_and_luminance`
    guard, same place (after TVI), same -EINVAL + message, both enc and
    source windows; LUT uploaded verbatim, never extended —
    `test_sycl_cambi_parity` window cases pin accept/reject set;
  - `CambiSyclSelect::k_rem[p + 1]` written by scan of pass p: no lane
    rewrites word another lane reads; bin 0 of pass 0 = exact zeros
    → `resolved`, later passes return on device;
  - histogram cells `uint16`, modular; order of updates free (true window
    counts), so run/skip rewrites keep bit-exactness.
  **On rebase**: `cambi.c` change to `c_value_pixel`,
  `calculate_c_values` window walk, `spatial_pooling`, preprocessing or
  `filter_mode` → mirror into device kernels same PR;
  `test_sycl_cambi_parity` asserts bit-exact per frame.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_cambi_sycl.cpp` | `cambi.c` | `test_sycl_cambi_parity.c` (bit-exact, 4 frames), `test_integer_cambi_sycl.c` (smoke) | [ADR-1357](../../../../../docs/adr/1357-sycl-cambi-device-resident.md) |

Second instance (ADR-1179, `fix/sycl-v1-model-crash`): `options_cambi_sycl`
lacked `cambi_high_res_speedup` (`hrs`). That knob carries
`VMAF_OPT_FLAG_FEATURE_PARAM`; its absence changed *serialised feature
name* — SYCL twin emitted `cambi_cmxv_17_vlt_0.06` while default
model `vmaf_v1.0.16_3d0h` asks for `cambi_hrs_1080_cmxv_17_vlt_0.06` —
prediction failed with `-EAGAIN` instead of falling through to default.
Two rebase-sensitive consequences: (1) every `VMAF_OPT_FLAG_FEATURE_PARAM`
knob of `cambi.c` must exist verbatim in `options_cambi_sycl`; (2)
`vmaf_feature_name_dict_from_provided_features()` must run in
`init_fex_sycl` **before** `enc_width` / `enc_height` / `enc_bitdepth`
default from picture geometry (same ordering as `cambi.c`),
else geometry defaults leak into feature name. TVI / VLT
tables come from shared `vmaf_cambi_init_tvi_and_vlt()` in `cambi.c`
— do not reintroduce private bisection in twin.

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_cambi_sycl.cpp` | `test_sycl_cambi_parity.c` (bit-exact per frame) + `test_integer_cambi_sycl.c` (smoke) | [ADR-0415](../../../../../docs/adr/0415-cambi-sycl-port.md), [ADR-1357](../../../../../docs/adr/1357-sycl-cambi-device-resident.md) |
