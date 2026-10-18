## Pelorus carrier form in libvmaf and patch 0017 (2026-10-17)

- `ffmpeg-patches/0017-libvmaf-read-pelorus-sidedata.patch`: the filter
  offers every `AV_FRAME_DATA_SEI_UNREGISTERED` entry of the distorted frame
  to `vmaf_set_perceptual_sidedata()`, newest first, and stops at the first
  accepted one. A refresh must not bring back the single
  `av_frame_get_side_data()` lookup; `core/test/test_ffmpeg_libvmaf_pelorus_scan_contract.py`
  refuses it. After a conflict run
  `python3 scripts/ci/ffmpeg_patch_stack.py --refresh`, then `--check`.
- `core/src/feature/perceptual_weight.c`: ingest unwraps the Pelorus 1.5
  carrier form with the vendored `pel_blob_unwrap()` before it parses the
  blob. Fork-only, nothing upstream to merge.
