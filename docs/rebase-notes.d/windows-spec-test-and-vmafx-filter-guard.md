## Windows spec test and vmafx filter header guard (2026-10-19)

- `scripts/codegen/vmafx_api/emit_ffmpeg_options.py` writes the guard
  `AVFILTER_VMAFX_OPTIONS_H`, the name FFmpeg's `fate-source`
  check derives from `libavfilter/vf_vmafx_options.h`. Regenerate
  `ffmpeg-patches/src/vf_vmafx_options.h` and refresh patch `0023` with
  `python3 scripts/ci/ffmpeg_patch_stack.py --refresh` rather than merging by
  hand; `HeaderGuards` in `scripts/ci/test_ffmpeg_patch_stack.py` checks every
  header a patch adds.
- `core/test/test_vmafx_spec.c` escapes `VMAFX_TEST_MODEL_DIR` with
  `spec_escape()` before using it in a spec. Fork-only, nothing upstream.
