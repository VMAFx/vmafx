- Refactor the DNN model loader and the PSNR feature bucket to the ADR-1142
  whole-tree clang-tidy ratchet. `core/src/dnn/model_loader.c` loses its four
  oversized functions: `vmaf_dnn_sidecar_load()` is split into one
  `parse_sidecar_*()` helper per sidecar field group,
  `vmaf_dnn_verify_signature()` into `default_registry_path()`,
  `resolve_bundle_abs()` and `run_cosign_verify()`, and the per-encoder preset
  ladder in `codec_block_preset_ordinal()` becomes a lookup table of
  (encoder family → preset ordinal) rows. `core/src/dnn/dnn_api.c` splits
  `vmaf_dnn_session_open()` into `resolve_load_path()` (the ADR-0174 int8
  sibling lookup) and `setup_luma_fast_path()` (the NCHW `[1,1,H,W]` scratch
  allocation), which also retires its `goto fail` ladder.
  `core/src/feature/psnr_tools.cpp` switches its pixel-format table to
  designated initialisers, and the psnr / float_psnr extractors declare their
  cross-TU registry linkage explicitly. Every touched `.c` file keeps
  upstream's `NULL` spelling behind a file-scoped ADR-1138
  `NOLINTBEGIN(modernize-use-nullptr)` bracket, because the required Windows
  MSVC lane compiles them with `cl.exe`.
  No user-visible behaviour changes: `--feature psnr --precision=max` on the
  `src01_hrc00` / `src01_hrc01` pair is byte-identical before and after
  (`fps`, a timing field, excluded), and the Netflix golden-data gate is
  unchanged.
