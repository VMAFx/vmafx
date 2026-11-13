---
paths:
  - core/src/cuda/common.h
  - core/src/sycl/common.h
invariant: GPU-parity matrix gates cross-backend consistency; HIP follows scaffolding; icpx wraps SYCL tidy.
area: backends-metrics-and-tests
---
<!-- markdownlint-disable MD013 MD060 -->
# GPU backend parity, HIP scaffolding, and SYCL toolchain

- **HIP backend scaffold contract** (fork-local, ADR-0212 / T7-10):
  `enable_hip=true` build path compiles
  [src/hip/](../src/hip/) and [src/feature/hip/](../src/feature/hip/)
  into `libvmaf_feature_static_lib`, exposes public C-API
  entry points in
  [include/libvmaf/libvmaf_hip.h](../include/libvmaf/libvmaf_hip.h)
  (`vmaf_hip_state_init` / `_import_state` / `_state_free` /
  `vmaf_hip_list_devices` / `vmaf_hip_available`). Until
  runtime PR (T7-10b) lands, every public entry point returns
  `-ENOSYS` and smoke test
  [test/test_hip_smoke.c](../test/test_hip_smoke.c) pins that
  contract. Any rebase or refactor "succeeding" scaffold
  (e.g. accidentally enabling code path) without flipping
  smoke expectations breaks rebase story for runtime PR.
  `dependency('hip-lang')` probe in
  [src/hip/meson.build](../src/hip/meson.build) stays
  `required: false` for scaffold; flipping to `true` belongs
  to runtime PR. `enable_hip` option type is
  `boolean` (matching `enable_cuda` / `enable_sycl`); never
  convert it to `feature` without ADR amendment per ADR-0212
  § "Decision".
- **GPU-parity matrix gate contract** (fork-local,
  [ADR-0214](../../docs/adr/0214-gpu-parity-ci-gate.md)).
  [`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py)
  is single source of truth for per-feature absolute
  tolerance every (CPU↔GPU, GPU↔GPU) cell must respect. CI
  job `vulkan-parity-matrix-gate` in
  [tests-and-quality-gates.yml](../../.github/workflows/tests-and-quality-gates.yml)
  runs it on every PR over CPU↔Vulkan/lavapipe; CUDA/SYCL/hardware-
  Vulkan are advisory until self-hosted runner exists. Never
  tighten `FEATURE_TOLERANCE` entry without measurement-driven
  follow-up ADR (per CLAUDE.md §12 r1). Adding new feature with
  GPU twin requires (1) `FEATURE_METRICS` entry, (2)
  `FEATURE_TOLERANCE` entry if feature relaxes places=4, and
  (3) row in
  [`docs/development/cross-backend-gate.md`](../../docs/development/cross-backend-gate.md).
- **icpx-aware clang-tidy wrapper for SYCL TUs** (fork-local,
  [ADR-0217](../../docs/adr/0217-sycl-toolchain-cleanup.md)).
  [`scripts/ci/clang-tidy-sycl.sh`](../../scripts/ci/clang-tidy-sycl.sh)
  is single entry point for linting `core/src/sycl/**` and
  `core/src/feature/sycl/**` files; it injects oneAPI SYCL
  include path + `-D__SYCL_DEVICE_ONLY__=0` so stock LLVM clang-tidy
  resolves `<sycl/sycl.hpp>`. CI lane
  `Tidy SYCL` in
  [`.github/workflows/lint-and-format.yml`](../../.github/workflows/lint-and-format.yml)
  runs wrapper over SYCL build tree; never invoke stock
  `clang-tidy` directly against SYCL TUs (will surface
  `'sycl/sycl.hpp' file not found` clang-diagnostic-errors). When
  adding new SYCL TU, no AGENTS.md update is needed — wrapper
  finds it via changed-file diff. Wrapper resolves icpx
  install via `$ICPX_ROOT` (override) or
  `/opt/intel/oneapi/compiler/latest` (default); if Intel
  reorganises this layout in future release, wrapper's candidate
  list needs new path added (see `for cand in ...` block in
  script). Companion bench-time helper:
  [`scripts/ci/sycl-bench-env.sh`](../../scripts/ci/sycl-bench-env.sh).
- **GPU long-tail terminus reached** (fork-local, T7-36 closure
  via [ADR-0210](../../docs/adr/0210-cambi-vulkan-integration.md)).
  Every registered feature extractor now has at least one GPU twin
  — cambi was last remaining gap. lpips remains ORT-delegated
  per [ADR-0022](../../docs/adr/0022-inference-runtime-onnx.md).
  Adding new feature extractor without same-PR GPU twin is now
  explicit choice — record deferral in ADR body.
  Governing batches:
  [ADR-0182](../../docs/adr/0182-gpu-long-tail-batch-1.md) (1) +
  [ADR-0188](../../docs/adr/0188-gpu-long-tail-batch-2.md) (2) +
  [ADR-0192](../../docs/adr/0192-gpu-long-tail-batch-3.md) (3).
