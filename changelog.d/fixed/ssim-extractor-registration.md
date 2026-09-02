- **`vmaf --feature ssim` could not resolve.** The fixed-point SSIM
  extractor `vmaf_fex_ssim` was defined in
  `core/src/feature/integer_ssim.c` but the source file was not
  listed in `core/src/meson.build`, and the symbol was not
  declared `extern` or referenced in `feature_extractor.c`'s
  `feature_extractor_list[]`. The result: `vmaf_get_feature_extractor_by_name("ssim")`
  returned `NULL` and `--feature ssim` silently produced no `ssim`
  metric block. Documented as a shipped feature in
  `docs/metrics/features.md` and reachable through the public CLI
  surface, so this was a user-discoverable hole, not an
  implementation detail. Surfaced by the partial-integration audit
  in `docs/research/0091-partial-integration-audit-2026-05-08.md`
  (PR #454). Fix wires `integer_ssim.c` into the build, adds the
  `extern` declaration + `&vmaf_fex_ssim` registry row in
  `feature_extractor.c`, and includes `config.h` in
  `integer_ssim.c` so the `VmafFeatureExtractor` struct layout
  agrees across translation units (the conditional `HAVE_CUDA` /
  `HAVE_SYCL` / `HAVE_VULKAN` members were previously visible to
  only one of the two TUs, tripping `-Wlto-type-mismatch` on
  Vulkan-enabled LTO links). New regression test
  `test_ssim_extractor_registered_and_extracts` in
  `core/test/test_feature_extractor.c` asserts both that the
  extractor resolves by name and that it appends a `ssim` score to
  the feature collector. The `docs/metrics/features.md` table row
  + footnote ² were also corrected — they previously claimed a
  Vulkan twin via T7-24, but the only Vulkan SSIM kernel
  (`core/src/feature/vulkan/ssim_vulkan.c`) defines
  `vmaf_fex_float_ssim_vulkan`, not a fixed-point twin.
