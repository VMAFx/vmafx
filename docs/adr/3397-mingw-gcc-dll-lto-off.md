<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3397: MinGW GCC DLL builds turn LTO off

- **Status**: Accepted
- **Date**: 2026-11-02
- **Deciders**: lusoris
- **Tags**: `build`, `meson`, `lto`, `ci`, `windows`

## Context

`core/meson.build` enables link-time optimisation by default (`b_lto=true`, `b_lto_threads=4`, [ADR-1172](1172-bound-lto-link-parallelism.md)) and builds both shared and static libraries (`default_library=both`). On Windows with MinGW GCC (`x86_64-w64-mingw32-gcc` 16.2.0, GNU `ld` 2.47), linking a DLL under LTO causes `ld` to leave LTO-compiled symbols at section `-1` (virtual address `0`, wrapping to an out-of-section export RVA `(0 - ImageBase) mod 2^32`) and causes `ld --out-implib` to emit import libraries (`src/libvmaf.dll.a`, `src/libvmafx.dll.a`) whose LTO-compiled C and C++ exports have `I __imp_<fn>` pointer slots and `I __nm_<fn>` data aliases, omitting `T <fn>` function thunks (`libvmaf-3.dll` / `libvmaf.dll.a`: 0 of 78 exports in `.text`, 0 `T` thunks; `libvmafx-1.dll` / `libvmafx.dll.a`: 52 typed function exports in `.text`, 48 `T` thunks from the `b_lto=false` AVX-512 static library, 4 missing `T` thunks on `vif_compute_line_residuals`, `vmaf_ceiln`, `vmaf_floorn`, `vmaf_log`, and 1129 LTO exports with out-of-section RVAs).

When `tools/vmaf.exe` links against `src/libvmaf.dll.a`, GNU `ld` resolves each direct function call (`call vmaf_picture_alloc`) against the auto-import symbol `__nm_vmaf_picture_alloc` (`__fu0_vmaf_picture_alloc`) and records a 32-bit PEI runtime pseudo-relocation per call site. Under 64-bit Windows high-entropy ASLR, the distance between the executable and `libvmaf-3.dll` exceeds 32 bits, and the MinGW-w64 runtime aborts before `main` (`Mingw-w64 runtime failure: 32 bit pseudo relocation at 00000001400053EF out of range`, exit code 3; VMAFx/vmafx#3257). Static builds (`--default-library=static`, as in the existing `Windows UCRT64` CI build) do not link against `.dll.a` import libraries and are unaffected.

## Decision

When `host_machine.system() == 'windows'`, `cc.get_id() == 'gcc'` or `cxx.get_id() == 'gcc'`, `get_option('b_lto')` is true, and `get_option('default_library') != 'static'`, `core/meson.build` sets `vmafx_effective_lto = false` (fed into `VMAFX_BUILD_FLAGS`, `vmafx_build_conf`, and `vmafx_build_id()` in `core/src/meson.build` so build provenance records the effective `b_lto=false` setting), adds `-fno-lto` via `add_project_arguments` and `add_project_link_arguments` for `c` and `cpp`, and emits an explicit configure `warning()` naming the substitution and its cause. Because Meson places its built-in `-flto=4` flag before project arguments on the compiler and linker command lines, the trailing `-fno-lto` wins without changing `default_options`, keeping [ADR-1172](1172-bound-lto-link-parallelism.md) in force on every other host and on static MinGW GCC builds. Pure Clang/lld targeting MinGW (`cc.get_id() == 'clang'` and `cxx.get_id() == 'clang'`) is left unchanged because it was not measured. `scripts/ci/check_mingw_import_thunks.py` pairs each `<build>/src/*.dll.a` with its DLL (`lib<name>.dll` or `lib<name>-*.dll`), parses the PE export directory and section headers in Python stdlib to collect function exports whose RVA lies in a `CODE` section (excluding data exports in `.data`/`.rdata` and the 2 untyped NASM symbols `vmaf_cpu_cpuid`, `vmaf_cpu_xgetbv` with COFF symbol type `0x0`), and checks that every function export has a `T <name>` thunk in `nm` output (measured non-LTO: `libvmaf.dll.a` 78/78 function exports with thunks, 0 missing; `libvmafx.dll.a` 1015/1015 function exports with thunks, 0 missing, 119 data exports excluded; measured LTO: `libvmaf.dll.a` 0 function exports, 0 thunks; `libvmafx.dll.a` 52 function exports, 48 thunks, 4 missing). The `Windows UCRT64` job in `.github/workflows/libvmaf-build-matrix.yml` builds a default-options shared/both tree (`core/build-shared`), checks its `.dll.a` import libraries against `core/build-shared/src`, and executes `core/build-shared/tools/vmaf.exe --help`. The `Tooling Tests` job in `.github/workflows/tests-and-quality-gates.yml` runs `scripts/ci/test_check_mingw_import_thunks.py` as part of the `tooling` suite.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| :--- | :--- | :--- | :--- |
| Turn `b_lto=false` globally in `default_options` | Simple one-line change | Loses the measured LTO speedup on Linux, macOS, and static MinGW builds ([ADR-1172](1172-bound-lto-link-parallelism.md)) | Unacceptable regression on unaffected platforms |
| Annotate every public and internal cross-DLL symbol with `__declspec(dllimport)` in consumer TUs | Avoids auto-import for annotated calls | Does not repair the broken `.dll.a` import library for downstream C callers that include standard headers without `dllimport` | Leaves `libvmaf.dll.a` without function thunks for external callers |
| Add `-fno-lto` with a configure `warning()` on MinGW GCC when `default_library != 'static'` (chosen) | Default `meson setup` produces working DLLs, `.dll.a` import libraries with `T` thunks, and runnable executables; static MinGW and non-Windows builds keep LTO; no silent fallback | Shared MinGW GCC builds do not get LTO | Chosen: fail-loud configure warning + working binaries + CI thunk check |

## Consequences

- **Positive**: Default `meson setup` on MinGW GCC (`default_library=both`, `b_lto=true`) emits `T <fn>` thunks in `libvmaf.dll.a` (78 `T`) and `libvmafx.dll.a` (1015 `T`), and `tools/vmaf.exe --help` starts cleanly (exit 0).
- **Negative**: Shared and `both` builds with MinGW GCC compile and link without LTO; the configure log prints a `WARNING:` explaining why and pointing to `--default-library=static` for LTO.
- **Neutral / follow-ups**: `scripts/ci/check_mingw_import_thunks.py` and `scripts/ci/test_check_mingw_import_thunks.py` guard `.dll.a` thunk generation; `Windows UCRT64` in `.github/workflows/libvmaf-build-matrix.yml` exercises the default shared/both build in CI; `docs/getting-started/building-on-windows.md` documents the behaviour.

## References

- Issue: VMAFx/vmafx#3257.
- Research digest: [Research-3397](../research/3397-mingw-gcc-lto-import-thunks.md).
- Prior ADR: [ADR-1172](1172-bound-lto-link-parallelism.md) (`b_lto=true`, `b_lto_threads=4` by default).
- Implementation: `core/meson.build`, `core/src/meson.build`, `scripts/ci/check_mingw_import_thunks.py`, `scripts/ci/test_check_mingw_import_thunks.py`, `.github/workflows/libvmaf-build-matrix.yml`, `.github/workflows/tests-and-quality-gates.yml`, `docs/getting-started/building-on-windows.md`.
