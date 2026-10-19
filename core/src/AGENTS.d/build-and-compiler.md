---
paths:
  - core/src/meson.build
  - core/test/check_exported_symbols.py
invariant: Windows nvcc host = build MSVC, else newest vswhere toolset, else PATH; C++ targets take vmaf_cppflags_common.
---
<!-- markdownlint-disable MD013 -->
# Build system, compiler discovery, and C++ profile invariants

## Windows CUDA compiler discovery

`meson.build` must assign `cl_path` on every discovery route. NVCC's `-ccbin`
and MSVC include discovery consume that same path. Order: build's own
MSVC (`nvcc_build_msvc`, from `cxx` when its id is `msvc`), then newest
toolset under latest `vswhere` install (sorted by `[version]`, never
first `cl.exe` of recursive walk: that was v142 toolset 14.29 of VS 18,
whose STL hides `<numbers>` from nvcc's C++20 host passes;
`T-WINDOWS-NVCC-CCBIN-OLDEST-TOOLSET-2026-10-06`), then `PATH`.
`nvcc_build_msvc` is assigned before `host_machine.system() == 'windows'`
block because regression extracts that block into project without
compilers and sets variable itself. Keep configure regression in
`../test/test_windows_cuda_compiler_discovery.py` when rebasing Windows
discovery block from Netflix PR #1472.

## C++ targets take `vmaf_cppflags_common` (ADR-0379)

- every C++ target linked into libvmaf passes `cpp_args : vmaf_cppflags_common` (`core/src/meson.build`). No per-target define lists.
- missing -> no `-fvisibility=hidden` -> internal symbols exported from `libvmaf.so` (72 did, until 2026-09-18); also no `HAVE_CUDA` / `HAVE_SYCL` -> `VmafPicturePrivate` layout skew (PR #840).
- `vmaf_cppflags_common` derived after last `vmaf_cflags_common +=`; new defines go before that line.
- gate: `python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C build check_exported_symbols` (`core/test/check_exported_symbols.py`).

## 10. `.cpp` files lint-clean to `modernize-*` profile (ADR-0915)

`.clang-tidy` enables full `modernize-*` family minus four explicit
opt-outs (`-modernize-use-trailing-return-type`, `-modernize-use-auto`,
`-modernize-avoid-c-arrays`, `-modernize-use-nodiscard`). CPU-built
`.cpp` translation units (`core/src/cpu.cpp`,
`core/src/feature/feature_collector.cpp`,
`core/src/metadata_handler.cpp`) are lint-clean to this profile.

When porting upstream Netflix `.c` patch onto one of these `.cpp`
files: prefer `nullptr` over `NULL`, prefer `<cstdlib>`/`<cstring>` over
`<stdlib.h>`/`<string.h>`, drop `<stdbool.h>` includes (in C++ `bool` is
keyword), and use `auto*` for `static_cast<T*>(malloc(...))`-style
initialisers where cast already spells type. These match checks
enabled by ADR-0915; deviating reintroduces warnings that touched-file
rule (ADR-0141) requires discharging in same PR.

## C++ placement new and delete visibility invariant (ADR-1337)

`core/src/meson.build` defines `vmaf_cppflags_common` including `-fvisibility-inlines-hidden` alongside `-fvisibility=hidden`. In GCC 16 C++26 mode, standard library placement new and delete (`_ZnwmPv`, `_ZdlPvS_`) are inline functions in `<new>` (`_GLIBCXX_PLACEMENT_CONSTEXPR`) that inherit default visibility from libstdc++ headers unless `-fvisibility-inlines-hidden` is applied (`-fvisibility=hidden` alone leaves inline functions visible).

**Load-bearing cross-platform compiler behavior**:

- **GCC & Clang (Linux ELF, Apple Clang Darwin Mach-O, MinGW PE/COFF)**: `-fvisibility-inlines-hidden` is supported across GCC (including GCC 16) and Clang (including Clang 22). It enforces hidden visibility on inline C++ standard library symbols, preventing `_ZnwmPv` and `_ZdlPvS_` from leaking into dynamic export table of `libvmaf.so` / `libvmaf.dylib`.
- **MSVC & clang-cl (`cxx.get_argument_syntax() == 'msvc'`)**: Windows MSVC toolchains govern exported symbols via explicit `__declspec(dllexport)` rather than ELF/Mach-O visibility flags. Meson's `cxx.get_supported_arguments(...)` evaluates compiler support and cleanly drops `-fvisibility-inlines-hidden`, avoiding invalid option warnings or build breaks while maintaining hidden visibility across ELF and Mach-O targets.

**Symbol gate & red cap**: `core/test/check_exported_symbols.py` runs on Linux shared builds. checker forbids globally allowlisting or suppressing C++ new/delete leaks. Instead, explicit red-cap check asserts `_ZnwmPv` and `_ZdlPvS_` are never exported, failing closed on leak regressions.

**Invariant for rebases and follow-up branches**: all C++ targets in `core/src/` must inherit `vmaf_cppflags_common` with `-fvisibility-inlines-hidden` preserved. No impact on public C API headers (`core/include/libvmaf/`), Netflix golden assertions, models, tuning, or score paths.
