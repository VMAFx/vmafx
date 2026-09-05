- Refactor the DNN model loader and the PSNR feature bucket to the ADR-1142
  whole-tree clang-tidy ratchet. `core/src/dnn/model_loader.c` loses its four
  oversized functions: `vmaf_dnn_sidecar_load()` is split into one
  `parse_sidecar_*()` helper per sidecar field group,
  `vmaf_dnn_verify_signature()` into `default_registry_path()`,
  `resolve_bundle_abs()` and `run_cosign_verify()`, and the per-encoder preset
  ladder in `codec_block_preset_ordinal()` becomes a lookup table of
  (encoder family → preset ordinal) rows. `core/src/feature/psnr_tools.cpp`
  switches its pixel-format table to designated initialisers, and the psnr /
  float_psnr extractors declare their cross-TU registry linkage explicitly.
  No user-visible behaviour changes: `--feature psnr --precision=max` on the
  `src01_hrc00` / `src01_hrc01` pair is byte-identical before and after
  (`fps`, a timing field, excluded), and the Netflix golden-data gate is
  unchanged.
