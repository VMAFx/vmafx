---
paths:
  - core/src/feature/cuda/cuda_tile_index.h
  - core/src/feature/cuda/integer_adm_cuda.c
invariant: LDG channel reads, tile indexing padding clamping, and unwind helper teardown.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Channel reads, tile indexing, and unwind helpers

- **Tile loads clamp padding indices** (`cuda_tile_index.h`): reflect once
  like CPU, then `vmaf_cuda_tile_index()` = identity for every consumed
  sample. Same rule as SYCL `sycl_tile_index.h`.
- Guards: `test_cuda_kernel_source_contract.py` (planted regression per
  rule above), `test_cuda_motion_tiny_frames`, `test_cuda_vif_min_dim`,
  `test_cuda_twin_option_parity`, `test_cuda_adm_dwt2_rows`,
  `test_cuda_float_motion_parity` (every frame, all three scores, `==`,
  8 / 10 / 12 bit), `test_float_motion_sad` (host helper, device-free).

## `__ldg()` pattern for VmafPicture channel reads (ADR-0762)

- **Extract typed `const uint8_t *__restrict__` (or `uint16_t *__restrict__` for
  16bpc) channel pointers from `VmafPicture` struct args BEFORE per-pixel body,
  then use `__ldg(&ptr[idx])` for all channel reads.**
  `calculate_ciede_kernel_8bpc` and `calculate_ciede_kernel_16bpc` in
  `integer_ciede/ciede_score.cu` = canonical examples: `VmafPicture` struct
  carries `void *data[3]`, which prevents compiler from seeing reads are
  alias-free when struct passed by value. Extracting typed `__restrict__`
  pointers at kernel entry makes invariant visible, routes 6 per-pixel
  channel reads through L1 read-only texture cache. Any future kernel reading
  per-pixel plane data from `VmafPicture` must follow same pattern.
  See [ADR-0762](../../../../../docs/adr/0762-cuda-ciede-ldg.md).
- **`*_unwind` / `*_init_unwind` helpers are single teardown path for
  their extractor, and arithmetic helpers must stay `static` in same
  translation unit (HISS-21 / 2026-09-21).** Every extractor's old `free_ref` /
  `free_buffers` / `fail_cuda` label now lives in one `static` helper that holds
  label's statements verbatim; callers pass their live `err` / `ret` so
  returned code is unchanged. `ssim_cuda.c`'s `issim_init_unwind` takes
  `ISSIM_UNWIND_*` stage because it replaced three-level fall-through cascade —
  adding resource means adding stage, not second exit path. helpers
  carrying score arithmetic (`integer_ssim_setup_geometry`'s `c1` / `c2`,
  `motion_v2_stamp_value`, `motion_v2_emit_frame`, `float_psnr_peak_for_bpc`)
  must stay `static` and keep each expression in one statement: moving operand
  across call boundary, or letting one of these become external, re-opens
  FMA-contraction divergence ADR-1253 closed. On rebase: reapply helper
  boundary, never restore label.
  `integer_adm_cuda.c` is out of scope for helper NAMES above: #1507
  rewrote its init and teardown while this branch was open, so it releases
  through paired helpers (`adm_cuda_init_device` / `adm_cuda_release_device`,
  `adm_cuda_init_buffers` / `adm_cuda_free_buffers`,
  `adm_cuda_load_modules` / `adm_cuda_unload_modules`) and keeps one
  `CHECK_CUDA_GOTO` ladder inside `adm_cuda_init_device_locked()`, same
  macro rest of CUDA tree uses. rules are unchanged for it:
  release set and release order on every exit path, real error code out of
  every failure, and no arithmetic expression split across boundary.
