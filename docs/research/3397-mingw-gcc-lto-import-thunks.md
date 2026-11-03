<!-- markdownlint-disable MD013 MD041 MD060 -->
# Research-3397: MinGW GCC LTO DLL import-library thunks and per-function verification

- **Status**: Active
- **Workstream**: [ADR-3397](../adr/3397-mingw-gcc-dll-lto-off.md)
- **Last updated**: 2026-11-02

## Question

Why does a default `meson setup` build of `tools/vmaf.exe` with MinGW-w64 GCC (`x86_64-w64-mingw32-gcc` 16.2.0, GNU `ld` 2.47) abort before `main` under 64-bit Windows ASLR, what symbol and export-table structures do LTO and non-LTO builds emit in `libvmaf-3.dll` / `libvmaf.dll.a` and `libvmafx-1.dll` / `libvmafx.dll.a`, and how should CI verify import-library function thunks without false positives on data exports or false negatives on single missing thunks?

## Sources

- Issue: VMAFx/vmafx#3257
- Decision record: [ADR-3397](../adr/3397-mingw-gcc-dll-lto-off.md)
- Prior LTO parallelism decision: [ADR-1172](../adr/1172-bound-lto-link-parallelism.md)
- Measured MinGW GCC 16.2.0 + GNU `ld` 2.47 build trees (`build/mingw-measure/lto/src` and `build/mingw-measure/nolto/src`)

## Findings

### 1. Runtime failure mechanism

Under `default_library=both` and `b_lto=true`, `tools/vmaf.exe` links against `src/libvmaf.dll.a`. When `libvmaf-3.dll` is linked with `-flto=4`, GNU `ld --out-implib` emits `I __imp_<fn>` import pointer slots and `I __nm_<fn>` auto-import data symbols for LTO-compiled C and C++ functions, omitting the `T <fn>` jump-stub thunks (`jmp *__imp_<fn>(%rip)`).

Without a `T <fn>` thunk in `libvmaf.dll.a`, GNU `ld` resolves each direct function call (`call vmaf_picture_alloc`) in `tools/vmaf.exe` against `__nm_vmaf_picture_alloc` (`__fu0_vmaf_picture_alloc`) and emits a 32-bit PEI runtime pseudo-relocation per call site. At startup under 64-bit high-entropy ASLR, the address delta between `vmaf.exe` (`0x140000000`) and `libvmaf-3.dll` (`0x37fac0000`) exceeds 32 bits, and the MinGW-w64 CRT aborts before `main`:

```text
Mingw-w64 runtime failure:
  32 bit pseudo relocation at 00000001400053EF out of range, targeting 000000037FAC3180, yielding the value 000000023FABDDD1.
```

(Exit code 3.)

### 2. Measured DLL export tables and `.dll.a` symbol counts

Inspection of the PE export directories (`.edata`), section headers, COFF symbol tables, and `nm` import-library symbols across `build/mingw-measure/nolto/src` (`-fno-lto`) and `build/mingw-measure/lto/src` (`-flto=4`) with `x86_64-w64-mingw32-gcc` 16.2.0 and GNU `ld` 2.47 gives:

| Build | DLL / Import Archive | Total `.edata` Exports | `.text` (`CODE`) Exports (COFF `ty 0x20`) | `.data` / `.rdata` Data Exports | Untyped NASM `.text` Exports (`ty 0x0`) | Out-of-Section `.edata` RVAs (`sec -1`) | `.dll.a` `__imp_` Slots | `.dll.a` `T` Thunks | Missing Function Thunks | Gate Result |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| `nolto` | `libvmaf-3.dll` + `libvmaf.dll.a` | 78 | 78 | 0 | 0 | 0 | 78 | 78 | 0 | PASS (`OK`) |
| `nolto` | `libvmafx-1.dll` + `libvmafx.dll.a` | 1136 | 1015 | 119 (86 `.data`, 33 `.rdata`) | 2 (`vmaf_cpu_cpuid`, `vmaf_cpu_xgetbv`) | 0 | 1136 | 1015 | 0 | PASS (`OK`) |
| `lto` | `libvmaf-3.dll` + `libvmaf.dll.a` | 78 | 0 | 0 | 0 | 78 (RVA `0x80540000`) | 78 | 0 | 0 (0 func exports) | FAIL (0 function exports) |
| `lto` | `libvmafx-1.dll` + `libvmafx.dll.a` | 1183 | 52 | 0 | 2 (`vmaf_cpu_cpuid`, `vmaf_cpu_xgetbv`) | 1129 (RVA `0x863e0000`) | 1183 | 48 | 4 (`vif_compute_line_residuals`, `vmaf_ceiln`, `vmaf_floorn`, `vmaf_log`) | FAIL (4 missing thunks) |

Key observations from the binary measurements:

1. **Out-of-section export RVAs under LTO**: When GNU `ld` constructs `.edata` for an LTO-linked DLL, symbols defined only in LTO IR still carry COFF section `-1` (`IMAGE_SYM_ABSOLUTE`, value `0`) and COFF symbol type `0x0`. Their export RVA is written as `(0 - ImageBase) mod 2^32` (`0x80540000` for `libvmaf-3.dll` at `ImageBase = 0x37fac0000`, `0x863e0000` for `libvmafx-1.dll` at `ImageBase = 0x279c20000`), which does not fall inside any section of the PE image.
2. **Non-LTO AVX-512 and referenced symbols in `libvmafx-1.dll`**: `libvmafx` links the AVX-512 static library built with `override_options: ['b_lto=false']` (`core/src/meson.build`). Those 48 non-LTO functions retain COFF section `1` (`.text`) and type `0x20` (`DT_FCN`), so `ld --out-implib` emits 48 `T` thunks for them. Four additional C/C++ functions (`vif_compute_line_residuals`, `vmaf_ceiln`, `vmaf_floorn`, `vmaf_log`) are referenced by non-LTO objects and land in `.text` with COFF type `0x20`, but lack `T` thunks in `libvmafx.dll.a`.
3. **Untyped NASM assembly labels**: `core/src/x86/cpuid.asm` defines `vmaf_cpu_cpuid` and `vmaf_cpu_xgetbv` via NASM `cglobal` (`x86inc.asm`), which emits `global <sym>` without a COFF `.type 32` (`DT_FCN`) directive. Both symbols have COFF type `0x0` in `libvmafx-1.dll`, so `ld --out-implib` treats them as data imports (`__imp_` + `__nm_`) in both `nolto` and `lto` builds; they are called only internally within `libvmafx` (`core/src/x86/cpu.c`) and in the statically compiled `test_x86_cpu_gate.c` mock.

### 3. Why a per-function check replaces the aggregate ratio

An earlier version of `scripts/ci/check_mingw_import_thunks.py` checked only `thunk_count >= imp_count * 0.5`. That aggregate ratio has three failure modes:

1. **False negative on partial missing thunks**: If 1 of 10 (or 4 of 52) function exports lacks a `T` thunk, the aggregate ratio (`0.900` or `0.923`) exceeds `0.5` and passes, even though any caller of the thunkless function auto-imports it as data and aborts under 64-bit ASLR.
2. **False positive on data-heavy DLLs**: Data exports in `.data` and `.rdata` produce `I __imp_<var>` slots without `T` thunks by design. A healthy DLL whose data exports exceed half of its total exports fails an `__imp_`-denominator ratio gate.
3. **Vacuous pass on zero-import or unparsed output**: Returning 0 when `imp_count == 0` allowed empty `nm` output or an unrecognized archive to pass silently.

Pairing each `<build>/src/*.dll.a` with its DLL (`lib<name>.dll` or `lib<name>-*.dll`), extracting the DLL's `.text` (`CODE`) function exports (with COFF type `0x20` when the COFF symbol table is present), requiring `len(func_exports) > 0`, and verifying that every function export has a `T <name>` thunk in `.dll.a` eliminates all three failure modes.

## Alternatives explored

- **Aggregate `T / __imp_ >= 0.5` ratio**: Rejected after review (PR #3262 T2, T4, T5, T10) for the three reasons above.
- **Shelling out to `objdump -p` and `objdump -h`**: Rejected in favour of parsing the PE headers, section table, export directory, and COFF symbol table directly with Python's standard `struct` module, avoiding an extra tool lookup while keeping zero third-party dependencies.

## Open questions

- None for MinGW GCC. Pure Clang/lld targeting MinGW (`cc.get_id() == 'clang'` and `cxx.get_id() == 'clang'`) remains unmeasured; mixed C/C++ configurations where either `cc` or `cxx` is GCC apply `-fno-lto`.

## Related

- ADRs: [ADR-3397](../adr/3397-mingw-gcc-dll-lto-off.md), [ADR-1172](../adr/1172-bound-lto-link-parallelism.md)
- PRs: #3262
- Issues: #3257
