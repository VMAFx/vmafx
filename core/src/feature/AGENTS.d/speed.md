---
paths:
  - core/src/feature/speed.c
  - core/src/feature/speed_cov.h
  - core/src/feature/speed_givens.h
  - core/test/test_speed_upstream_form.c
invariant: SpEED buffer allocation, chroma dimensions, anti-alias decimation, covariance sums, Netflix's fp64 expressions.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# SpEED Buffers, Chroma Geometry, and Decimation

- **SPEED and CAMBI feature decomposition and NULL preservation** (fork-local,
  ADR-1146): [`cambi.c`](../cambi.c), [`cambi.h`](../cambi.h), [`speed.c`](../speed.c),
  [`speed_qa.c`](../speed_qa.c), and SIMD twins in [`x86/`](../x86/)
  (`cambi_avx2.c`, `cambi_avx512.c`, `speed_avx2.c`, `speed_avx512.c`) are
  decomposed into static single-purpose helpers to satisfy
  `readability-function-size` (≤60 lines, max nesting 4). C TUs preserve `NULL`
  through file-scoped `/* NOLINTBEGIN(modernize-use-nullptr) */` /
  `/* NOLINTEND(modernize-use-nullptr) */` brackets (ADR-1138). **On rebase**:
  when merging upstream changes to `calculate_c_values`, `init`, or
  `est_params`, map changes into respective decomposed helpers
  (`c_values_*`, `validate_and_setup_dimensions`, `alloc_cambi_buffers`,
  `solve_covariance_system`) rather than re-inlining. Shared prototypes in
  `cambi_internal.h` and `speed_internal.h` must not change without mirroring
  to all GPU/CPU twins (verified by `scripts/ci/twin-drift-check.sh`). Numerical
  bit-exactness is governed by ADR-1146.

## SpEED math = Netflix's fp64 expressions (ADR-1477)

Three places of [`speed.c`](../speed.c) are fp64 arithmetic rounded to `float`
once, as Netflix master has them (`libvmaf/src/feature/speed.c`, `9e48141b`):

- `create_givens()` (:418, :423): `float s1 = 1.0 / sqrt(1 + t * t);`
- `update_entropy()` (:802): `log2(L * S[..] + sigma_nn) + log2(2 * M_PI * M_E)`
- `get_speed_score()` (:897-:928): `log2(1 + variance)`, `(a + b) / 2.0`,
  `0.75 * a + 0.25 * b`, product in fp64

Port #213 had `sqrtf` / `log2f` / `0.75f` there: `speed_chroma` up to 2.3e-5,
`speed_temporal` up to 6.6e-4, `vmaf_v1.0.16` up to 2.5e-5 from Netflix. Never
bring fp32 forms back; `performance-type-promotion-in-math-fn` on these lines
= NOLINT citing ADR-1477, not rewrite. **On upstream sync**: upstream's side
on these three. `sqrtf` of fp32 norm and `/ 2.0f` (#1209) stay: same value.

Mirrors that change in same PR:

- `si_create_givens()` in [`speed_internal.c`](../speed_internal.c).
- `speed_internal_gpu_tail_scores()` (`speed_internal.c`) = `update_entropy()`,
  `est_params()` steps 8-9, `get_speed_score()` and `speed_extract_score()`'s
  one-side-singular zero. GPU twins call it at collect time; same library,
  same flags, same libm as `speed.c`.
- [`speed_givens.h`](../speed_givens.h) `speed_givens_unit()` = rotation's
  fp64 statement in fp32 for device kernels. Input domain = 2^23 + 1 floats of
  [1, 2] (`|t| <= 1`); proven on every one. Change -> rerun
  `test_speed_upstream_form`; mismatch = wrong routine, never tolerance
  or exception table.

Guard: `core/test/test_speed_upstream_form.c` (device-free). Every libc:
`create_givens()`, `update_entropy()`, `get_speed_score()` (reached through
`speed_internal_cpu_*()` test entries after `get_speed_score()` in
`speed.c`, declared in `speed_internal.h`; keep them on sync) == Netflix's
statements transcribed with file:line and evaluated with host's own
`sqrt()` / `log2()`; Givens exhaustive; host tail == same statements, modes
0-6, singular rule, bad arguments. glibc only:
CPU extractors == Netflix master's values on `testdata/*_576x324_48f.yuv` (11
frames, 4 option sets; measured with glibc 2.44); other libcs print check
as skipped with reason. Variant `test_speed_upstream_form_foreign_libm`
(`-DVMAF_TEST_ASSUME_FOREIGN_LIBM`) runs non-glibc path on every lane.
Reference values come from Netflix build; never regenerate them from
fork, never pin host-libm value outside glibc guard.

## `speed_chroma` / `speed_temporal` are float-build-only

two upstream Speed extractors register inside
`#if VMAF_FLOAT_FEATURES` block in `feature_extractor.c`. They
are absent from default `meson setup` build; users who want
them must pass `-Denable_float=true`. Do **not** lift them out
of `#if` block — they call into Speed-specific helpers
in `vif_tools.c` that are themselves only compiled in
float-features path.

[ADR-0253](../../../../docs/adr/0253-speed-qa-extractor.md)
(Proposed) records deferral on extending this surface with
SpEED-QA full-frame reduction or SpEED-driven model. Status quo
is binding contract until one of three named triggers in
that ADR fires.

## `speed_temporal` frame buffers hold `alloc_height` rows (Netflix/vmaf#1626)

`speed_temporal` `init()` in [`speed.c`](../speed.c) sizes its four frame
buffers `float_stride * dimensions.alloc_height`. `filter_and_downscale()`
copies `alloc_height` rows out of each buffer and resamples frame back in
place at `scaled_height`; `speed_prescale` above 1 makes that taller than
source. Upstream still allocates `float_stride * h` (issue #1626, fix proposed
upstream in PR #1627). **On upstream sync**: keep `alloc_height`;
restoring `h` brings heap overrun back.
`test_speed_frame_buffers` (1.0 / 1.5 / 2.0 / 4.0) catches it under ASan only.
Drop this note once upstream fix is ported. `speed_chroma` and
CUDA / HIP / SYCL twins already size from scaled geometry.

## `speed_chroma` buffers use `speed_chroma_dimensions()` ceiling extents

In subsampled formats (4:2:0, 4:2:2) odd luma width or height produces
extra chroma row or column to cover last luma sample
(`vmaf_chroma_extent()`). `speed.c` `init_chroma()`, `speed_chroma_cuda.c`,
and `speed_chroma_hip.c` must derive chroma extents via
`speed_chroma_dimensions()`, not integer `/ 2`, so that buffers match
ceiling dimensions allocated by `picture.c` and copied by `picture_copy()`.

## SpEED anti-alias + decimation: fused on every target (Netflix/vmaf 76ea5f03, ad42c532, 9cb9479f)

`filter_and_downscale()` in [`speed.c`](../speed.c) + mirror
`speed_internal_filter_and_downscale()` in [`speed_internal.c`](../speed_internal.c):
`vif_filter1d_dec16_s()` ([`vif_tools.c`](../vif_tools.c)) into scratch
plane, then row-wise copy of `downscaled_w` floats back into frame buffer, on
every target (x86 included since ad42c532). Vertical pass at every 16th row,
horizontal pass at every 16th column via `vif_filter1d_horizontal_dec16_s()`.

- Vertical pass: `vif_filter1d_vertical_dispatch_s()`. x86 + AVX2 +
  `fwidth <= MAX_FWIDTH_AVX_CONV` (same gate as `vif_filter1d_s()`'s AVX2
  convolution): mirrored row pointers from `vif_mirror_index()`, then
  `convolution_f32_avx_rows_s()` ([`common/convolution_avx.c`](../common/convolution_avx.c));
  else scalar `vif_filter1d_vertical_s()`. One lane = one column, products
  added in tap order to sum starting at 0 = scalar order. AVX-512 host runs
  AVX2 kernel.
- Horizontal decimated pass: scalar only, one implementation for every
  target. Upstream's `convolution_f32_avx_dec16_s()` carries its own scalar
  copy of it; fork does not (HISS-19). Upstream's `VMAF_NO_FUSE` asm barrier
  not taken: every TU builds with contraction off (ADR-1461).
- Upstream's `vif_filter1d_dec16_scalar_s()` split not taken: CPU mask
  selects scalar (`vmaf_set_cpu_flags_mask(0)`), tests use that.

Invariant: `vif_filter1d_dec16_s()` output == scalar `vif_filter1d_s()` +
`vif_dec16_s()` bit for bit (same taps, mirror, accumulation order) at every
dispatch level. `test_speed_filter` (`memcmp`): fused scalar, fused with host
flags, and old x86 two-call path with host flags (AVX2 convolution, aligned
layout only: it loads aligned) against scalar two calls; whole-frame
`speed_internal_filter_and_downscale()` at both masks; `test_avx_rows`
holds `convolution_f32_avx_rows_s()` to scalar sum on every column of widths
1..41 (decimated pass never reads row's last columns, so masked tail only
seen there). After any edit to scalar passes in `vif_tools.c`: run
`test_speed_filter` on aarch64 cross build under qemu. Change
`vif_filter1d_horizontal_s()` or mirror -> change
`vif_filter1d_horizontal_dec16_s()` in same PR. Change one of `speed.c` /
`speed_internal.c` here -> change other one too. Change to the vertical tap
order -> change `convolution_f32_avx_rows_s()` in same PR.

GPU twins (`cuda/speed/speed_score.cu`, `hip/speed/speed_hip_device.h`,
`sycl/speed_sycl_pipeline.cpp`) and Rust twin (`rust/feature/speed`,
`filter_dec16`) evaluate filter at decimated samples only, in scalar
arithmetic; x86 port needed comment edits only.

Test entries `speed_internal_cpu_est_params()` and
`speed_internal_cpu_compute_eigenvalues()` (after `get_speed_score()` in
`speed.c`, declared in `speed_internal.h` with `SpeedInternalEstGeometry`)
serve `core/test/test_speed_chroma.c` (Netflix c70debb10: `est_params()`,
`compute_eigenvalues()`, `get_speed_score()` against upstream's values at
1e-4). Upstream's test includes `speed.c`; fork's may not
(`check-no-non-header-includes`). **On upstream sync**: keep entries;
signature change of `est_params()` / `compute_eigenvalues()` changes them in
same PR.

## SpEED covariance sums: row kernels return scalar's bits (ADR-1459)

[`speed_cov.h`](../speed_cov.h) = contract. Reference: `compute_cov_kernel_scalar()`
in [`speed.c`](../speed.c): one running `double` sum, row-major, per element two
subtractions, one multiply, one add, each rounded alone. Product sits in own
statement + function-scoped no-contraction guard (GCC `optimize` attribute,
clang `fp contract(off)` pragma). Keep both on upstream sync: upstream body
`result += (val_x - mean_x) * (val_y - mean_y);` gets fused by clang on aarch64.

`compute_covariance_matrix()` walks lower triangle row by row of y blocks
(`compute_covariance_row()`); `speed_cov_row_fn` kernel returns up to
`SPEED_COV_ROW_MAX` (= block size 5) sums per call. Kernels:
`speed_cov_row_scalar` (count reference calls), `x86/speed_avx2.c`,
`x86/speed_avx512.c`, `arm64/speed_neon.c`. Lane = one sum; no lane feeds
another; no FMA. `test_speed_simd` (`memcmp`) on x86 and under qemu-aarch64.

Upstream `compute_cov_kernel_avx2` / `_avx512` (30f472b14) and `_neon`
(15297286) split one sum over lanes with FMA: not bit-exact, removed here.
**On upstream sync**: never re-import; port upstream changes to
`compute_covariance()` into `compute_covariance_row()` by hand.

Kernel reads up to `width + SPEED_COV_ROW_MAX - 1` floats per row of `data_y`
whatever `count`: SpEED's block grid always holds those floats. New caller -> same
guarantee or scalar kernel.

SpEED built + registered whatever `enable_float` says (Netflix/vmaf
4718b4f5f meson hunk + 6046b1926): `speed.c`, `speed_internal.c`,
`vif_tools.c`, `common/convolution.c` sit in unconditional
`libvmaf_feature_sources` list of `core/src/meson.build`;
`vmaf_fex_speed_chroma` / `_temporal` sit outside `#if VMAF_FLOAT_FEATURES` in
`feature_extractor.cpp`. Default model `vmaf_v1.0.16_3d0h` reads
`speed_chroma`; GPU twins link `speed_internal.c`. **On upstream sync**: never
move them back under float gate. SpEED tests in `core/test/meson.build`
carry no `enable_float` gate; `test_speed` fails in `-Denable_float=false`
build when extractors drop out.

Bilinear prescale column table (Netflix/vmaf 78e11b52c): `speed_init()` fills
`SpeedBuffers.bilinear_x1a` / `_x2a` / `_dxa` once when prescale is
bilinear and resamples; `speed_prescale_frame()` uses it through
`vif_scale_frame_bilinear_precomputed_s()`. Same bits as
`vif_scale_frame_s(vif_scale_bilinear, ...)`, which `speed_internal.c` keeps
calling (`test_vif_bilinear` holds both to per-pixel scaler). No width
limit: `vif_tools.c` walks columns in chunks of 1024; never import upstream's
`VIF_BILINEAR_MAX_WIDTH` stack table, assert or init refusals.
