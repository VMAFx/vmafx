- Renamed include-guard macros in eleven fork-added headers from the
  C-reserved form `__VMAF_<X>_H__` to the conformant form
  `VMAF_<X>_H_`, clearing CERT-C DCL37-C / `bugprone-reserved-identifier`
  warnings on touched files (ADR-0885). Files updated:
  `core/include/libvmaf/{libvmaf_sycl,macros,vmaf_assert}.h`,
  `core/src/{dict_internal,thread_locale}.h`,
  `core/src/feature/arm64/ms_ssim_decimate_neon.h`,
  `core/src/feature/x86/ms_ssim_decimate_{avx2,avx512}.h`,
  `core/src/sycl/{common,dmabuf_import,picture_sycl}.h`.
  Upstream-mirror headers and the three public headers cited in
  ADR-0278 (`feature.h`, `model.h`, `dnn.h`) are intentionally left
  untouched to preserve rebase compatibility and existing NOLINT
  citations. No ABI, API, or build-flag change — guards are local
  preprocessor symbols only.
