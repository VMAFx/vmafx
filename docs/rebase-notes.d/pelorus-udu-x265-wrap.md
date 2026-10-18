## Pelorus side data behind the x265 UUID (2026-10-18)

- `core/src/feature/perceptual_weight.c::unwrap_payload()` calls
  `vmaf_pelorus_udu_classify()` (`core/src/feature/pelorus_udu_wrap.h`) and
  drops the outer 16 bytes only for `VMAF_PELORUS_UDU_X265_WRAPPED`
  (ADR-3340). Fork-only code; no upstream file changes. The Pelorus mirror
  (`core/src/interop/pelorus_*.c`, `core/include/libvmaf/pelorus/`) is not
  touched. `testdata/pelorus/udu-wrap-vectors.txt` is Pelorus' file byte for
  byte; `test_pelorus_udu_wrap` and `test_perceptual_weight` guard it.
