- **docs(libvmaf)**: add Doxygen-style WHY-non-obvious doc blocks for the
  14 functions flagged by CodeQL `cpp/poorly-documented-function`
  (alerts #259, #261, #262, #265, #408, #409, #410, #411, #412, #413,
  #414, #416, #734, #746). Covers the upstream-mirror ADM kernels
  (`integer_compute_adm`, `adm_dwt2_s123_combined`, `init`,
  `adm_dwt2_8_avx512`, `adm_dwt2_s123_combined_avx512`), the AVX2 /
  AVX-512 motion convolution twins (`y_convolution_8/16_avx2/avx512`,
  `motion_score_pipeline_8_avx512`), the VIF subsample-readout twins
  (`vif_subsample_rd_8_avx2/avx512`), the SSIMULACRA2 AVX-512
  YUV→linear-RGB port (`ssimulacra2_picture_to_linear_rgb_avx512`), and
  the CAMBI feature-extractor `init`. Each block documents the
  bit-exactness invariant (ADR-0138 / ADR-0139) or the upstream-parity
  invariant (ADR-0141) that prevents refactoring, plus the caller
  contract a future maintainer would need from the SIMD dispatch site.
  The 15th alert (#269, `test_feature.c`) gets a per-instance
  `lgtm[cpp/poorly-documented-function]` comment with justification —
  the file is already covered by `paths-ignore` in
  `.github/codeql-config.yml` but the existing alert needs an inline
  acknowledgement to clear on the next scan. Supersedes the
  global-suppression sibling PR.
