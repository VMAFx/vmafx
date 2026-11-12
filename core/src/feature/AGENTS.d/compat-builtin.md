---
paths:
  - core/src/feature/compat_builtin.h
invariant: MSVC builtin clz shim must use _BitScanReverse, never __lzcnt.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# MSVC Builtin Bit Scan Shim Compatibility

## `compat_builtin.h`: never `__lzcnt` (ADR-1166)

MSVC `__builtin_clz` / `__builtin_clzll` shim must use `_BitScanReverse` /
`_BitScanReverse64`. `__lzcnt` emits LZCNT instruction unconditionally with
no runtime feature gate. On x86-64 without ABM/LZCNT `F3` prefix is
ignored and it retires as BSR, returning MSB index instead of
leading-zero count. Silently wrong VIF and ADM shifts result, with no
fault and no CI signal (every hosted Windows runner has LZCNT).
Netflix/vmaf#1422 proposes
`__lzcnt` form; Netflix/vmaf#1551 is upstream's own retraction of it.
`scripts/ci/check-msvc-clz-shim.sh` fails `fast` suite if it comes back.
