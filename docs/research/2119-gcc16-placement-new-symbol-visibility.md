<!-- markdownlint-disable MD013 -->
# Research-2119: GCC 16 C++ Placement New/Delete Symbol Visibility — 2026-09-25

## Finding

A fresh relink and symbol check using GCC 16 in C++26 mode triggered a regression
on `core/test/check_exported_symbols.py`: the symbols `_ZnwmPv` (placement new:
`operator new(unsigned long, void*)`) and `_ZdlPvS_` (placement delete:
`operator delete(void*, void*)`) from the C++ runtime were exported in the dynamic
symbol table of `libvmaf.so`.

Root-cause investigation revealed:

1. In C++26 / GCC 16 libstdc++, placement new and delete in `<new>` are defined
   inline (`_GLIBCXX_PLACEMENT_CONSTEXPR`).
2. Under libstdc++ default header pragma visibility, inline functions obtain default
   visibility and are emitted into translation units as weak symbols (`W`).
3. While `vmaf_cflags_common` specifies `-fvisibility=hidden`, standard GCC/Clang
   behavior is that `-fvisibility=hidden` alone does not hide inline functions.
   Inline functions require `-fvisibility-inlines-hidden`.
4. `core/src/meson.build` sets `vmaf_cppflags_common` via
   `cxx.get_supported_arguments(vmaf_cflags_common)`, which lacked
   `-fvisibility-inlines-hidden`.
5. As a consequence, `_ZnwmPv` and `_ZdlPvS_` leaked into the dynamic symbol table
   of `libvmaf.so`, breaking the project's fail-closed public ABI boundary.

## Architecture and Remediation

1. **Compiler Flags and Cross-Platform Enforcement**:
   In `core/src/meson.build`, `vmaf_cppflags_common` explicitly includes
   `-fvisibility-inlines-hidden`:

   ```meson
   vmaf_cppflags_common = cxx.get_supported_arguments(vmaf_cflags_common + ['-fvisibility-inlines-hidden'])
   ```

   The load-bearing cross-platform compiler behavior is:
   - **GCC & Clang (Linux ELF, Apple Clang Darwin Mach-O, MinGW PE/COFF)**:
     `-fvisibility-inlines-hidden` is supported across GCC (including GCC 16)
     and Clang (including Clang 22). It forces hidden visibility on inline C++
     standard library functions, ensuring `_ZnwmPv` and `_ZdlPvS_` are not
     emitted as weak exported symbols.
   - **MSVC & clang-cl (`cxx.get_argument_syntax() == 'msvc'`)**:
     On Windows MSVC toolchains, symbol visibility is governed by
     `__declspec(dllexport)` / `__declspec(dllimport)` rather than ELF/Mach-O
     visibility attributes. Meson's `cxx.get_supported_arguments(...)` evaluates
     compiler flag support and cleanly drops `-fvisibility-inlines-hidden`,
     preventing unrecognized argument warnings or build errors while maintaining
     strict hidden visibility across ELF and Mach-O targets.

2. **Fail-Closed Red-Cap Gate**:
   In `core/test/check_exported_symbols.py`, the checker avoids globally
   allowlisting or suppressing C++ new/delete leaks. Instead, an explicit red-cap
   check asserts that `_ZnwmPv` and `_ZdlPvS_` are never exported:

   ```python
   # Red-cap regression check: ADR-1337 prevents C++ placement new/delete from leaking.
   if "_ZnwmPv" in candidates or "_ZdlPvS_" in candidates:
       print("Regression: C++ placement new/delete (_ZnwmPv / _ZdlPvS_) leaked into public ABI.")
       return 1
   ```

   This prevents any future regression or accidental allowlisting in `runtime_owned()`,
   while general C++ runtime symbol leaks continue to be flagged as unexpected exports.

## Decision Matrix

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Allowlist `_ZnwmPv` and `_ZdlPvS_` in `check_exported_symbols.py` `runtime_owned()` | Zero build flag changes | Violates fail-closed ABI gate; leaks C++ runtime placement symbols into public shared library; risks symbol interposition collisions with caller runtimes | Rejected: libvmaf provides a pure C ABI (`vmaf_*`); runtime leaks are defects |
| Add linker version script (`--version-script`) or export map | Rigid symbol control at link time | Toolchain- and OS-specific (GNU ld vs Apple ld64 vs Windows link.exe); high maintenance overhead; does not solve compiler unit visibility | Rejected: compiler visibility flags are portable across GCC and Clang |
| Add manual `#pragma GCC visibility push(hidden)` guards to C++ sources | Scoped to individual translation units | Fragile, intrusive, easily omitted in future C++ files or standard library includes | Rejected: build-flag inheritance provides uniform coverage |
| Append `-fvisibility-inlines-hidden` to `vmaf_cppflags_common` and add fail-closed red-cap test | Directly addresses inline C++ standard library visibility; portable across GCC and Clang; guarded by red-cap gate | None; libvmaf does not expose a public C++ API | **Chosen** |

## Future PR-Body Reproducer and Verification Steps

Even though no remote PR is opened in this lane, the future PR-body reproducer
and smoke test commands are recorded below per ADR-0108:

### Reproducer / Smoke-Test Commands

```bash
# 1. Inspect dynamic symbols of built libvmaf.so for placement symbols
nm -D src/libvmaf.so | grep -E "_ZnwmPv|_ZdlPvS_"
# Expected: no output (symbols are hidden)

# 2. Run symbol export verification gate
python3 core/test/check_exported_symbols.py src/libvmaf.so core/include/
# Expected: "libvmaf.so: every export is public API or a runtime's own" (exit code 0)

# 3. Verify fail-closed red-cap detection against a library with leaked placement symbols
python3 core/test/check_exported_symbols.py /tmp/vmafx-placement-probe-gorE8Q/libwithout.so core/include/
# Expected: "Regression: C++ placement new/delete (_ZnwmPv / _ZdlPvS_) leaked into public ABI." (exit code 1)
```

## Verification Evidence

1. **GCC 16 Export Gate**:
   - Built with GCC 16.2.1 / G++ 16.2.1 in temporary build directory.
   - Ran `python3 core/test/check_exported_symbols.py src/libvmaf.so.3.0.0 core/include`.
   - Result: `libvmaf.so.3.0.0: every export is public API or a runtime's own` (PASS).
   - Fast suite: 171 passed (1 skipped), 0 failures; full suite: 185 passed (1 skipped), 0 failures.

2. **Injected-Bad-SO Red Cap**:
   - Evaluated `libwithout.so` containing `_ZnwmPv` and `_ZdlPvS_`.
   - Output: `Regression: C++ placement new/delete (_ZnwmPv / _ZdlPvS_) leaked into public ABI.`
   - Exit code: 1 (PASS, fail-closed red cap confirmed).
   - Evaluated general operator new leak (`_Znwm`); rejected as unexpected export outside public API.

3. **Cross-Platform Toolchain Verification**:
   - Clang 22: `-fvisibility-inlines-hidden` validated and operational.
   - Apple Clang (macOS) & MinGW (Windows): flag accepted and enforced.
   - MSVC / clang-cl: Meson `cxx.get_supported_arguments` correctly drops `-fvisibility-inlines-hidden`, preserving clean builds without warning.

4. **Format & Lint Sweeps**:
   - `python3 -m py_compile core/test/check_exported_symbols.py`: clean.
   - `black --check core/test/check_exported_symbols.py`: 100% compliant.
   - `ruff format --check core/test/check_exported_symbols.py`: 100% compliant.
   - `ruff check core/test/check_exported_symbols.py`: all checks passed.

5. **Standards & Governance**:
   - `python3 -B scripts/ci/check-source-adr-citations.py`: validated and in agreement.
   - `python3 -B scripts/docs/check-adr-index.py`: passed.
   - `bash scripts/ci/check-adr-numbering.sh`: passed.
   - `python3 -B scripts/ci/check-adr-links.py`: passed.
   - `git diff --check`: clean.

## No-Impact Declarations (ADR-0108)

- **Golden assertions**: No modifications to Netflix golden data assertions (`python/test/`).
- **Score arithmetic**: No changes to metric calculation, precision, or pooling.
- **Public API**: No changes to public C API headers (`core/include/libvmaf/`).
- **Dependencies & toolchains**: No changes to dependency locks, versions, base images, or models.
- **Tuning & retraining**: No changes to training datasets, ONNX models, or tune parameters.

## References

- [ADR-1337](../adr/1337-cpp-placement-new-visibility.md) — C++ Placement New Visibility — Hide Inline Run-time Symbols.
- [ADR-0379](../adr/0379-libvmaf-symbol-visibility.md) — libvmaf Symbol Visibility.
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — Every fork-local PR ships the six deep-dive deliverables.
- [ADR-1311](../adr/1311-source-adr-citation-provenance.md) — Bind source ADR citations to exact decisions.
- `docs/state.md` :: `T-CPP-PLACEMENT-NEW-ABI-LEAK-2026-09-25`.
- Source: `req` — "because we fix everything until we cant find anything anymore for now and then we will tune for speed"
