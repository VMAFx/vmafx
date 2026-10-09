## motion2 / motion3 derived frame by frame

`rc4/api-motion-incremental`, [ADR-2090](adr/2090-motion-window-incremental.md),
amends [ADR-2074](adr/2074-vmafx-window-scores.md) decision 9.

- `core/src/feature/integer_motion.c`: the flush body is split into
  `motion_window_stamp()`, `motion_window_count_sads()`,
  `motion_window_derive()`, `vmaf_motion_window_advance()` and
  `vmaf_motion_window_flush()`; `motion_flush_one()` is unchanged. An upstream
  sync of `flush()` (Netflix `integer_motion.c`) ports the per-frame
  statements into `motion_flush_one()` and the stamp into
  `motion_window_stamp()`, never back into one loop in `flush()`: the
  advance would then miss them. `integer_motion_v2.c` follows.
- `motion_window.h`: `VmafMotionWindow` gains `state`
  (`VmafMotionWindowState`); every caller of `vmaf_motion_window_flush()`
  (CPU, CUDA, SYCL, HIP, Metal motion twins) also registers `.advance`.
  `test_motion_window_advance_contract.py` lists the TUs.
- `core/src/feature/feature_extractor.h`: `VmafFeatureExtractor` gains the
  optional `advance` callback after `flush`. A descriptor copied whole (the
  Rust twin shim of the RC4 Rust lanes) inherits it and must set or clear it.
- `core/src/libvmaf.c`: `vmaf_engine_read_pictures()` is split into
  `read_pictures_owned()` and a wrapper that calls `advance_extractors()`;
  `vmaf_read_pictures_sycl()` and `fence_for_read()` call it too, and
  `vmaf_engine_advance()` exposes it to the VMAFx completion thread
  (`advance_engine()` in `core/src/vmafx/window.c`, engine lock, before each
  pass). An upstream sync of `vmaf_read_pictures()` ports into
  `read_pictures_owned()`.
  `vmaf_engine_feature_score_at_index()` fences on `-EINVAL` for a fed frame.
- Tests whose expectations moved: `test_score_pooled_eagain`
  (`score_pooled(i - 1, i - 1)` after `read_pictures(i)` now 0),
  `test_vmafx_window` (motion windows complete at step 4),
  `test_gpu_float_ssim_auto_scale_contract.py` (reads `read_pictures_owned()`).
- No golden-data impact: every motion value is the flush-time value bit for
  bit (32 988 per-frame values against master, 0 different; Netflix golden
  gate green); no FFmpeg patch change (`libvmaf.h` unchanged, the scores only
  arrive earlier).
- Rust twins (lane request MI-1, Q-093): `VmafxRsTwin` gains `advance` (last
  field; `VMAFX_RS_ABI_VERSION` stays 1, no release has shipped the Rust ABI);
  on a conflict in `core/src/rust/include/vmafx_rs.h` take either side and run
  `scripts/dev/rust-abi-header.sh`. The shim maps `advance` to
  `twin_advance()`, never the inherited C callback, and
  `advance_one_extractor()` (`core/src/libvmaf.c`) initialises a pooled Rust
  twin before its first advance. `motion_rust`'s `window.rs` ports
  `motion_window_stamp()`, `motion_window_count_sads()`,
  `motion_window_derive()`, `vmaf_motion_window_advance()` and
  `vmaf_motion_window_flush()` statement by statement: an upstream sync that
  changes them changes `window.rs` in the same PR
  (`test_rust_motion_window_incremental`, `rust_twin_diff.py --feature motion`).
