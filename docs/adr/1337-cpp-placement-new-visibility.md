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

We update `vmaf_cppflags_common` in `core/src/meson.build` to explicitly
include `-fvisibility-inlines-hidden` alongside the existing
`-fvisibility=hidden`.

Additionally, we added a red-cap regression check directly to
`core/test/check_exported_symbols.py` that specifically asserts `_ZnwmPv`
and `_ZdlPvS_` are not leaked, ensuring they are never inadvertently
allowlisted in the future.

## Consequences

- **Positive**: `libvmaf.so` restores its strict adherence to the public
  API surface. Silent symbol interposition risks from standard library
  placement new/delete are eliminated.
- **Positive**: The gate is hardened via a red-cap regression test against
  future C++ standard library internal changes.
- **Negative**: No negative impacts on downstream consumers, as no public
  C++ ABI exists for libvmaf.

## References

- [ADR-0379](0379-libvmaf-symbol-visibility.md) — libvmaf Symbol Visibility
