- **Cambi performance cluster — port nine upstream commits
  ([ADR-0328](../docs/adr/0328-cambi-cluster-port-skip-shared-header-rename.md)).**
  Ports Netflix upstream's 2026-05-06 / 2026-05-07 cambi optimisation
  cluster (`d655cefe` … `984f281f`, nine commits) onto the fork. CAMBI
  runs measurably faster on every CPU path with no change to scores or
  CLI surface; the Netflix-golden checkerboard + src01 assertions still
  pass to the same precision. Highlights:
  - Reciprocal-LUT-based `c_value_pixel` (replaces a per-pixel float
    division with a table lookup).
  - 1-D row-prefix-sum + column-add factoring of the spatial-mask SAT
    recurrence.
  - Compact `v_band_size × width` histogram layout (only the value band
    that's actually queried by the c-value gate gets a row).
  - Per-pixel skip for histogram updates outside the useful value band
    (bit-identical with the unskipped path).
  - Fused `uh_slide` / `uh_slide_edge` middle-phase subtract+add (cancels
    when leaving and entering pixels are in-band with the same value).
  - AVX-2 implementations of `decimate`, `filter_mode`, and the per-row
    `calculate_c_values_row` (fully gather-based, with a per-chunk
    early-out when the mask is all-zero).
  - Frame-level `VmafCalcCValues` dispatch (replaces the previous
    per-row callback) with a `CAMBI_CALC_C_VALUES_BODY` macro that
    keeps the scalar / AVX-2 / fork-local NEON drivers in lockstep.

  The tenth upstream commit (`41bacc83` "move shared code to cambi.h")
  is intentionally skipped per ADR-0328 — the fork's macro-based
  dispatch already keeps the helper inlines internal to `cambi.c`, and
  introducing a second cambi header would collide with the fork's
  `cambi_internal.h` (Vulkan-twin shim API). AVX-512 calc_c_values_row
  and a true NEON calc_c_values_row are tracked as perf follow-ups in
  `docs/rebase-notes.md` (RN-2026-05-08-cambi-cluster); both fall back
  to bit-exact paths today, only throughput is affected.
