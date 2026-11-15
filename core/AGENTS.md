<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — core

Generated index. Pick area for touched paths (both if 2 fit); match `Touching`,
read each matching page first. Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../docs/development/agents-index.md)).

Orientation for any coding agent working inside `core/`. Root orientation
lives in [../AGENTS.md](../AGENTS.md); this file is scoped hand-off for
this subtree. Claude Code equivalents in [../CLAUDE.md](../CLAUDE.md).

## Scope

C engine — VMAF metric, feature extractors, backends, public API,
CLI (`tools/vmaf`, `tools/vmaf_bench`), and C unit tests.

```text
core/
  include/libvmaf/   # public C API (libvmaf.h, dnn.h, model.h, picture.h, ...)
  src/               # engine + feature extractors + backends
    cuda/            # CUDA backend runtime (picture, dispatch, ring buffer)
    sycl/            # SYCL backend runtime (queue, USM, dmabuf import)
    dnn/             # ONNX Runtime integration (tiny AI)
    feature/         # per-feature CPU implementations
      x86/           # AVX2 / AVX-512 SIMD paths
      arm64/         # NEON SIMD paths
      cuda/          # CUDA feature kernels
      sycl/          # SYCL feature kernels
  test/              # C unit tests (µnit-style: test.h + mu_run_test)
  tools/             # vmaf CLI, vmaf_bench, cli_parse
  meson.build
  meson_options.txt
```

## Ground rules for this subtree

- **Coding standards**: NASA/JPL Power of 10 + JPL-C-STD + SEI CERT C (see
  [../docs/principles.md](../docs/principles.md)). `.clang-tidy` enforces.
- **License headers**: Netflix-header-preserving for upstream-touched files;
  `Copyright 2026 Lusoris` for wholly-new files.
  See [ADR-0861](../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)
  (supersedes ADR-0025 line format).
- **Style**: K&R, 4-space, 100-char columns, `.clang-format` authoritative.
- **Banned functions** (see `docs/principles.md §1.2 rule 30`): `gets`,
  `strcpy`, `strcat`, `sprintf`, `strtok`, `atoi`, `atof`, `rand`, `system`.
- **Every non-void return value is checked or explicitly `(void)`-discarded.**
- **Every new file starts with license header** (Netflix preserved on
  upstream-touched; Lusoris on wholly-new — see ADR-0861).
- **Fixed-width integer printf formatting uses `<inttypes.h>` PRI macros**
  (`PRId64`, `PRIu64`, `PRIx64`, `PRIu32`, …) — never `(unsigned long)` +
  `%lu` or `(long long)` + `%lld` for `uint64_t` / `int64_t`.
  `(unsigned long)` form silently truncates on Windows LLP64 (32-bit
  `unsigned long`); PRI macros expand correctly on every supported data
  model. CERT FIO47-C, MISRA 21.6, [ADR-0876](../docs/adr/0876-printf-format-portability-pri-macros.md).
  Non-fixed-width POSIX types (`off_t`, `pid_t`, `time_t`) keep
  `(long long)` + `%lld` cast idiom (or `(intmax_t)` + `%jd`); Windows
  `DWORD` keeps `(unsigned long)` + `%lu` (cast spells type
  exactly).

## Areas

| Area | Read first | Covers |
| --- | --- | --- |
| Backends, metric contracts, and tests | [backends-metrics-and-tests](AGENTS-backends-metrics-and-tests.md) | core/src/cuda/*, core/src/sycl/*, core/src/vmafx/*, core/src/feature/{motion,ms_ssim,speed,vif,psnr}.c, core/test/*, Vulkan frame import, parity harnesses, and testdata/bench_all.sh. |
| Build, toolchains, and compiler flags | [build-and-toolchains](AGENTS-build-and-toolchains.md) | core/meson.build, core/meson_options.txt, core/src/meson.build, core/tools/meson.build, core/test/meson.build, core/src/check_device_compression.py, core/test/test_strict_fp_compiler_args.py, core/test/test_icx_system_libm.py, core/test/test_device_code_compression.py, .clang-tidy; strict-FP flags, icx/icpx libm, device compression, Windows UTF-8 paths. |
| Engine lifecycle, models, Rust, and MCP | [engine-and-runtime](AGENTS-engine-and-runtime.md) | core/include/libvmaf/*, core/src/libvmaf.c, model.c, predict.c, picture*, thread_pool*, fex_ctx_vector*, core/src/feature/*.c, core/src/mcp/*, core/src/rust/**, core/api/vmafx.toml, core/tools/cli_parse.cpp, and core/tools/vmaf.cpp. |
