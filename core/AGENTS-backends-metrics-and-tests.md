<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Backends, metric contracts, and tests: `core/` agent invariants

core/src/cuda/*, core/src/sycl/*, core/src/vmafx/*,
core/src/feature/{motion,ms_ssim,speed,vif,psnr}.c, core/test/*, Vulkan frame
import, parity harnesses, and testdata/bench_all.sh.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `/testdata/bench_all.sh`, `/testdata/perf_multi_resolution.json` | [backend-engagement-and-benchmarking](AGENTS.d/backend-engagement-and-benchmarking.md) | Backend selection flags verify engagement per run; benchmark harness maintains thread and stderr hygiene. |
| `src/cuda/common.h`, `src/sycl/common.h` | [gpu-backends-and-parity](AGENTS.d/gpu-backends-and-parity.md) | GPU-parity matrix gates cross-backend consistency; HIP follows scaffolding; icpx wraps SYCL tidy. |
| `src/feature/motion.c`, `src/feature/ms_ssim.c`, `src/feature/speed.c` | [metric-contracts-motion-and-speed](AGENTS.d/metric-contracts-motion-and-speed.md) | MS-SSIM honors enable_lcs; float_motion surfaces extra options; SpEED singular is non-fatal. |
| `src/feature/vif.c`, `src/feature/psnr.c` | [metric-contracts-psnr-and-vif](AGENTS.d/metric-contracts-psnr-and-vif.md) | integer_vif is luma-only across backends; psnr_max separates scale from peak; float VIF avoids AVX-512. |
| `test/meson.build`, `test/test_motion_avx512_parity.c` | [test-harnesses-and-parity](AGENTS.d/test-harnesses-and-parity.md) | Fuzz harnesses track coverage; required aggregator rejects removed float_ansnr; AVX-512 verifies parity. |
| `src/vmafx/frame_import_vulkan.c`, `src/cuda/import_vulkan.c`, `test/vmafx_vulkan_producer.c`, `test/vmafx_vulkan_test_util.h`, `test/test_vmafx_import_vulkan*` | [vmafx-vulkan-frames](AGENTS.d/vmafx-vulkan-frames.md) | Library makes no Vulkan call; CUDA imports opaque memory + timelines, SYCL/HIP take the dma-buf route; refusals named. |
