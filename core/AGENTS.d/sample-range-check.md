---
paths:
  - core/src/picture_sample_range.c
  - core/src/picture_sample_range.h
  - core/src/libvmaf.c
  - core/include/libvmaf/libvmaf.h
  - core/tools/cli_parse.cpp
  - core/tools/vmaf.cpp
invariant: Samples above 2^bpc - 1 are invalid input; only opt-in read_pictures check scans for them.
---
<!-- markdownlint-disable MD013 MD060 -->
# Sample range contract and the opt-in check (ADR-1918)

- Caller contract: every sample of `bpc`-bit picture is at most
  2^bpc - 1. Out-of-range input is invalid; CPU extractors and their GPU
  twins may score it differently
  (T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05). Do not widen twin's
  integers for out-of-range samples: contract and check close it.
- `vmaf_set_sample_range_check_enabled()` sets `VmafContext::check_sample_range`
  (off by default). `read_pictures_validate_and_prep()` calls
  `vmaf_picture_check_sample_range()` for both pictures after
  `validate_pic_params()` and before any extractor runs; with flag off it
  reads no sample. 8- and 16-bit pictures return at once; device picture
  returns `-ENOTSUP`.
- CLI: `--check-sample-range` / `--check_sample_range` -> `CLISettings::check_sample_range`
  -> `init_cli_context()` calls setter right after `vmaf_init()`.
- FFmpeg filters do not expose it (decoder and scaler output is in range;
  ADR-1918 alternatives); no `ffmpeg-patches/` change.
- Tests: `core/test/test_sample_range_check.c` (public API, every plane, both
  pictures, off by default), `test_check_sample_range_flag` in
  `core/test/test_cli_parse.c`.
