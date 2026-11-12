---
paths:
  - core/src/feature/iqa/convolve.c
  - core/src/feature/iqa/ssim_tools.c
invariant: IQA/VIF SIMD helper decomposition and reserved-identifier cleanup invariants.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# IQA SIMD Helper Decomposition and Renames

- **IQA / VIF SIMD helper decomposition** (fork-local, ADR-0146):
  `iqa_convolve` in
  [`iqa/convolve.c`](../iqa/convolve.c) is split into
  `iqa_convolve_horizontal_pass` + `iqa_convolve_vertical_pass`
  composed by `iqa_convolve_1d_separable` (for `IQA_CONVOLVE_1D`)
  and `iqa_convolve_2d`; `iqa_ssim` in
  [`iqa/ssim_tools.c`](../iqa/ssim_tools.c) is split into
  `ssim_workspace_alloc` / `_free` + `ssim_compute_stats` +
  `ssim_init_args` around explicit `struct ssim_workspace`;
  `vif_statistic_s_avx2` in
  [`x86/vif_statistic_avx2.c`](../x86/vif_statistic_avx2.c) is split
  into `vif_stat_simd8_compute` + `vif_stat_simd8_reduce` around
  explicit `struct vif_simd8_lane` that carries `__m256` lane
  state between two halves. **Load-bearing**: per-lane
  scalar-float reduction via 32-byte aligned `tmp_n[8]` / `tmp_d[8]`
  in `vif_stat_simd8_reduce` preserves ADR-0139 exactly;
  convolve pass ordering in `iqa_convolve_1d_separable` preserves
  ADR-0138 exactly. On rebase: if upstream rewrites any of these
  three functions, prefer upstream's shape **only** if it maintains
  both invariants; otherwise keep fork's split and re-document
  divergence in
  [rebase-notes 0039](../../../../docs/rebase-notes.md). Also:
  TU-static rename `_calc_scale` → `iqa_calc_scale` in
  `iqa/convolve.c` is fork-local — keep on rebase. See
  [ADR-0146](../../../../docs/adr/0146-nolint-sweep-function-size.md).
- **IQA reserved-identifier rename** (fork-local, ADR-0148):
  every `_iqa_*` / `struct _kernel` / `_ssim_int` /
  `_map_reduce` / `_map` / `_reduce` / `_context` /
  `_ms_ssim_*` / `_ssim_*` / `_alloc_buffers` /
  `_free_buffers` symbol and four underscore-prefixed
  header guards (`_CONVOLVE_H_`, `_DECIMATE_H_`,
  `_SSIM_TOOLS_H_`, `__VMAF_MS_SSIM_DECIMATE_H__`) was renamed
  to its non-reserved spelling. IQA tree is now baseline
  lint-clean. **Load-bearing NOLINTs** (do not collapse on
  rebase): scoped
  `NOLINTBEGIN/END(clang-analyzer-security.ArrayBound)` around
  inner kernel loops in `ssim_accumulate_row` and
  `ssim_reduce_row_range` of
  [`integer_ssim.c`](../integer_ssim.c) —
  `k_min`/`k_max` clamping is provably correct but
  analyzer can't follow it across helper boundary; scoped
  `NOLINTBEGIN/END(clang-analyzer-unix.Malloc)` around
  `check_simd_variant` and `check_case` in
  [`../../test/test_iqa_convolve.c`](../../../test/test_iqa_convolve.c)
  — test exits process on failure path; small allocations
  leak by design at test end; cross-TU
  `NOLINTNEXTLINE(misc-use-internal-linkage)` on `compute_ssim`
  in [`ssim.c`](../ssim.c) and `compute_ms_ssim` in
  [`ms_ssim.c`](../ms_ssim.c) — declared in `ssim.h` /
  `ms_ssim.h`, called from `float_ssim.c` /
  `float_ms_ssim.c`; clang-tidy runs per-TU and can't see
  bridge. On rebase, keep all these brackets verbatim. See
  [ADR-0148](../../../../docs/adr/0148-iqa-rename-and-cleanup.md)
  and [rebase-notes 0041](../../../../docs/rebase-notes.md).
- **Lockstep SIMD twin updates**: changes to `iqa/convolve.c`, `iqa/ssim_tools.c`,
  or `iqa/ssim_accumulate_lane.h` ship in lockstep with x86 and ARM64 SIMD twins
  in same PR; see [twin-update-rules](../arm64/AGENTS.d/twin-update-rules.md).
