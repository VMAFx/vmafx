---
paths:
  - core/src/feature/sycl/integer_psnr_sycl.cpp
  - core/test/test_sycl_psnr_parity.c
invariant: integer_psnr_sycl.cpp = full CPU psnr option table, bit-exact; ceiling division for chroma plane geometry.
area: motion-psnr-and-moment
---
<!-- markdownlint-disable MD013 MD060 -->
# Integer PSNR extractor and kernels

- **`integer_psnr_sycl.cpp` honours `enable_chroma` option parity**
  (ADR-0453). `enable_chroma` option (default `true`) clamps `n_planes`
  to 1 in `init_fex_sycl` when set to `false`, matching CPU
  `integer_psnr.c::init`'s behaviour. On rebase: keep clamp and
  `default_val.b = true` aligned with CUDA and Vulkan twins; all three
  backends must agree on default and dispatch logic.
- **`integer_psnr_sycl.cpp` = full CPU `psnr` option table, bit-exact**
  ([ADR-1365](../../../../../docs/adr/1365-sycl-twin-cpu-option-parity.md)).
  Device reduces per-plane SSE only. `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak`, `min_sse`, `uncapped` act on host through
  `feature/psnr_score.h` (`vmaf_psnr_peak` / `_max` / `_from_mse` /
  `_aggregate`) — same helpers as CPU `integer_psnr.c`. `emit_plane()`
  order = CPU (`psnr_*`, then `mse_*`); `collect()` folds SSE + sample
  count into `apsnr_*` totals, `flush_fex_sycl()` publishes aggregates
  after final collect. **On rebase**: no local copy of PSNR math; keep
  option table = CPU table (names, defaults, range, no `FEATURE_PARAM`).
  Guard: `test_sycl_twin_option_parity`.
- **`integer_psnr_sycl.cpp` uses ceiling division for chroma plane geometry**
  (PR #878 Vulkan twin fix). `cw` and `ch` computed via
  `(w + 1U) >> 1` / `(h + 1U) >> 1`, not `w / 2U` / `h / 2U`, to match
  CPU + CUDA + Vulkan behaviour on odd-dimension YUV420. On rebase: if
  upstream Netflix changes chroma-dimension formula in
  `integer_psnr.c::init`, propagate here and to CUDA and Vulkan twins
  in same PR.
- **`integer_psnr_sycl.cpp` SSE: group reduction, 32-bit squares**
  (ADR-1369). `launch_sse()` = 16 pixels per work-item one grid apart,
  `reduce_over_group`, one atomic per group; `psnr_item_sse<uint32_t>` only
  while 16 squares fit (bpc <= 12). Chroma from the shared planes.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_psnr_sycl.cpp` | `integer_psnr.c` | `test_sycl_psnr_parity.c` | ADR-0868 (round 1) |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_psnr_sycl.cpp` | `test_sycl_psnr_parity.c` | [ADR-0868](../../../../../docs/adr/0868-gpu-backend-kernel-coverage.md) |
