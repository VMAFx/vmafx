- **VMAF v1.0.16 SDR models** (ported from Netflix upstream
  [`4718b4f5f`](https://github.com/Netflix/vmaf/commit/4718b4f5f)): ships the new
  v1 generation of SDR models. Four standard variants —
  `vmaf_v1.0.16_3d0h` (1080p 3H), `vmaf_v1.0.16_5d0h` (phone 5H),
  `vmaf_v1.0.16_1d5h_2160` (4K 1.5H), `vmaf_v1.0.16_3d0h_2160` (4K 3H,
  [0, 110] range) — live under `model/vmaf_v1.0.16/`, and four
  high-frame-rate variants (`vmaf_v1.0.16_hfr_*`) live under
  `model/vmaf_v1.0.16_hfr/`. All eight are registered as built-ins
  (`--model version=vmaf_v1.0.16_3d0h`) and embedded into `libvmaf` at build
  time, alongside the existing v0 models. New documentation at
  [`docs/models/v1.md`](https://github.com/VMAFx/vmafx/blob/master/docs/models/v1.md).
  The four non-HFR models are fully supported on the CPU path; the four `_hfr`
  variants are shipped but not yet runnable on the fork (they need the
  `motion_five_frame_window` 5-frame plumbing deferred per ADR-0337). The
  upstream golden test `python/test/vmaf_v1_quality_runner_test.py` (46
  assertions) is added verbatim.
- Restored two Python-harness wirings dropped in the ADR-0700
  `python/vmaf`→`compat/python-vmaf` migration that the v1.0.16 models need:
  the `VmafexecQualityRunner.FEATURES` list (`adm3`/`motion3`/`cambi`/
  `speed_chroma_uv`) and the CAMBI enc-override param flow
  (`call_vmafexec` + `_generate_result` append `:cambi.enc_width/height/
  bitdepth=` from the asset, matching upstream). C engine unchanged;
  cambi scores unchanged (key-name only).
