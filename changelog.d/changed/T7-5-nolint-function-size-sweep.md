- T7-5 — readability-function-size NOLINT sweep. Refactored
  `float_adm.c::extract` (debug-feature appends extracted into
  `append_debug_features` helper) and `tools/vmaf.c::main` (eight
  helpers extracted: `open_input_videos`, `init_gpu_backends`,
  `allocate_model_arrays`, `model_label`, `load_model_collection_entry`,
  `load_one_model_entry`, `configure_tiny_model`, `resolve_tiny_device`,
  `skip_initial_frames`, `run_frame_loop`, `report_pooled_scores`).
  Two pre-2026-04-21 historical-debt NOLINTs removed; remaining NOLINTs
  in `tools/vmaf.c` (`copy_picture_data`, `init_gpu_backends`, `main`)
  carry inline justification per ADR-0141 §2 — load-bearing CLI
  cleanup-ownership chain and conditional-compilation backend stanzas
  that further extraction would obscure. Netflix CPU golden assertions
  byte-exact (90/90 + 57/57 VMAF-specific tests pass; pre-existing
  pypsnr/niqe Python-3.14 failures unchanged). Closes T7-5.
