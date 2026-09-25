<!-- markdownlint-disable MD013 -->
# ADR-1337: C++ Placement New Visibility — Hide Inline Run-time Symbols

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris fork maintainers
- **Tags**: `build`, `api`, `security`, `abi`, `fork-local`

## Context

A fresh relink using GCC 16 in C++26 mode triggered a regression on
`core/test/check_exported_symbols.py`: the symbols `_ZnwmPv` (placement new)
and `_ZdlPvS_` (placement delete) from the C++ runtime were suddenly exported
into the public ABI of `libvmaf.so`.

ADR-0379 previously enforced `-fvisibility=hidden` on C compiles
(`vmaf_cflags_common`) to prevent silent symbol interposition. However,
`vmaf_cppflags_common` did not include `-fvisibility-inlines-hidden` because
no public C++ API surface existed at the time.

In C++26, placement new and delete are declared as `constexpr` in `<new>`.
Due to the default visibility pragma in `libstdc++` headers, these inline
functions receive default visibility and are emitted as weak symbols. Since
`-fvisibility=hidden` does not override the visibility of inline functions,
these symbols leaked into the dynamic symbol table, violating our fail-closed
ABI gate.

## Decision

We update `vmaf_cppflags_common` in `core/src/meson.build` via
`cxx.get_supported_arguments(vmaf_cflags_common + ['-fvisibility-inlines-hidden'])`
to explicitly include `-fvisibility-inlines-hidden` alongside the existing
`-fvisibility=hidden`.

Cross-platform compiler behavior is handled cleanly:
- On GCC (including GCC 16) and Clang (including Clang 22, Apple Clang on
  macOS, and MinGW GCC/Clang on Windows), `-fvisibility-inlines-hidden` is
  supported and enforced, hiding inline standard library symbols and preventing
  `_ZnwmPv` and `_ZdlPvS_` from leaking into dynamic export tables.
- On Windows MSVC and clang-cl (`cxx.get_argument_syntax() == 'msvc'`), symbol
  visibility is governed by `__declspec(dllexport)` rather than ELF/Mach-O
  visibility flags. Meson's `cxx.get_supported_arguments(...)` probes compiler
  support and cleanly drops `-fvisibility-inlines-hidden` without warning or
  build failure.

Additionally, we added a fail-closed red-cap regression check directly to
`core/test/check_exported_symbols.py` that specifically asserts `_ZnwmPv`
and `_ZdlPvS_` are not leaked, ensuring they are never inadvertently
allowlisted or suppressed in the future.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Allowlist `_ZnwmPv` and `_ZdlPvS_` in `check_exported_symbols.py` `runtime_owned()` | Zero build flag changes | Violates fail-closed ABI gate; leaks C++ runtime placement symbols into public shared library; risks symbol interposition collisions with caller runtimes | Rejected: libvmaf provides a pure C ABI (`vmaf_*`); runtime leaks are defects |
| Add linker version script (`--version-script`) or export map | Rigid symbol control at link time | Toolchain- and OS-specific (GNU ld vs Apple ld64 vs Windows link.exe); high maintenance overhead; does not solve compiler unit visibility | Rejected: compiler visibility flags are portable across GCC and Clang |
| Add manual `#pragma GCC visibility push(hidden)` guards to C++ sources | Scoped to individual translation units | Fragile, intrusive, easily omitted in future C++ files or standard library includes | Rejected: build-flag inheritance provides uniform coverage |
| Append `-fvisibility-inlines-hidden` to `vmaf_cppflags_common` and add fail-closed red-cap test | Directly addresses inline C++ standard library visibility; portable across GCC and Clang; guarded by red-cap gate | None; libvmaf does not expose a public C++ API | **Chosen** |

## Consequences

- **Positive**: `libvmaf.so` restores its strict adherence to the public
  API surface. Silent symbol interposition risks from standard library
  placement new/delete are eliminated.
- **Positive**: The gate is hardened via a red-cap regression test against
  future C++ standard library internal changes.
- **Negative**: No negative impacts on downstream consumers, as no public
  C++ ABI exists for libvmaf.
- **Neutral**: No changes to Netflix golden assertions, models, snapshots,
  tuning, benchmarks, retraining, public C API headers (`core/include/libvmaf/`),
  CLI flags, or GPU kernels.

## References

- [ADR-0379](0379-libvmaf-symbol-visibility.md) — libvmaf Symbol Visibility.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — Every fork-local PR ships the six deep-dive deliverables.
- [ADR-1311](1311-source-adr-citation-provenance.md) — Bind source ADR citations to exact decisions.
- [Research-2119](../research/2119-gcc16-placement-new-symbol-visibility.md) — GCC 16 C++ placement new/delete symbol visibility investigation, red-cap verification, and evidence.
- `docs/state.md` :: `T-CPP-PLACEMENT-NEW-ABI-LEAK-2026-09-25`.
- Source: `req` — "because we fix everything until we cant find anything anymore for now and then we will tune for speed"
