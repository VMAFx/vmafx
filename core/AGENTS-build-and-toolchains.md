<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Build, toolchains, and compiler flags: `core/` agent invariants

core/meson.build, core/meson_options.txt, core/src/meson.build,
core/tools/meson.build, core/test/meson.build,
core/src/check_device_compression.py,
core/test/test_strict_fp_compiler_args.py, core/test/test_icx_system_libm.py,
core/test/test_device_code_compression.py, .clang-tidy; strict-FP flags,
icx/icpx libm, device compression, Windows UTF-8 paths.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `/docs/principles.md`, `/.clang-tidy` | [coding-standards-and-lint](AGENTS.d/coding-standards-and-lint.md) | Public entry points carry VMAF_EXPORT; C++23 adheres to safety invariants; NOLINT cites ADR within one line. |
| `src/meson.build`, `meson_options.txt`, `src/check_device_compression.py`, `test/test_device_code_compression.py` | [device-code-compression](AGENTS.d/device-code-compression.md) | Every device compile takes its backend's compression list; the build refuses raw device code. |
| `src/meson.build`, `test/test_icx_system_libm.py` | [host-libm-link-policy](AGENTS.d/host-libm-link-policy.md) | Every icx / icpx link gets -no-intel-lib=libimf; host math comes from glibc libm. |
| `src/meson.build`, `test/meson.build`, `/dev/Containerfile` | [hybrid-gcc-icpx-toolchain](AGENTS.d/hybrid-gcc-icpx-toolchain.md) | CC=gcc CXX=icpx builds give icpx C++ the strict spelling and link SYCL-on tests as C++. |
| `meson.build`, `/scripts/ci/run_meson_test.py` | [meson-build-and-environment](AGENTS.d/meson-build-and-environment.md) | Meson strips secret tokens from test environment, pins version >= 1.4.0, and tracks SYCL headers. |
| `meson.build`, `tools/meson.build` | [orientation-and-workflows](AGENTS.d/orientation-and-workflows.md) | Orientation for core C engine workflows, governing ADRs, backend subtree orientation, and build targets. |
| `src/meson.build`, `test/test_strict_fp_compiler_args.py` | [strict-fp-compiler-args](AGENTS.d/strict-fp-compiler-args.md) | Strict FP is project argument; every C and C++ translation unit builds without contraction. |
| `meson.build`, `src/meson.build` | [windows-path-contract](AGENTS.d/windows-path-contract.md) | Windows internal paths enforce UTF-8 normalization and runtime shims. |
