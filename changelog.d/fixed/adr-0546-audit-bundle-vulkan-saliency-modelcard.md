- **Vulkan-01**: `vmaf_get_feature_extractor_by_name("integer_motion_vulkan")` now
  returns the correct extractor on Vulkan-enabled builds. The symbol
  `vmaf_fex_integer_motion_vulkan_impl` was declared extern in
  `feature_extractor.c` but was never added to `feature_extractor_list[]`;
  the entry is now present under `#if HAVE_VULKAN` (ADR-0546).

- **saliency-tune-01**: `vmaf-tune recommend-saliency --saliency-aware` now exits
  with code 2 and a structured error message when `--encoder` names a codec
  without saliency ROI support (NVENC, QSV, AMF, VideoToolbox, libvpx-vp9, etc.).
  The previous behaviour was a silent WARNING + plain encode. To restore the old
  graceful-fallback, pass `--saliency-fallback-plain` or set
  `VMAFTUNE_SALIENCY_FALLBACK_OK=1` (ADR-0546).

- **ai-01**: Synthetic-stub model cards no longer contain the literal word
  `PLACEHOLDER` in the Sigstore signing note. The replacement wording is
  "not applicable (synthetic-stub model card; production models are signed via
  Sigstore — see docs/development/release.md)" (ADR-0546).
