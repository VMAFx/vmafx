---
paths:
  - core/src/x86/avx512_warm_up.h
  - core/src/cpu.cpp
  - core/test/test_cpu.c
  - core/test/test_inline_asm_clobber_contract.py
invariant: Inline asm that clobbers ymmN / zmmN also names xmmN; clang drops wide clobber without target feature.
---
<!-- markdownlint-disable MD013 -->
# Inline assembly clobber lists

- Only inline asm in `core/src`: `x86/avx512_warm_up.h::vmaf_x86_avx512_warm_up()` (`vpxord zmm0`), called by `cpu.cpp::vmaf_init_cpu()` on AVX-512 hosts.
- Clobber list = `"xmm0", "zmm0"`. Never `"zmm0"` alone: clang drops clobber of register enclosing function's target lacks (no caller built with `-mavx512f`) -> value caller holds in `xmm0` zeroed. GCC unaffected (same hard register).
- Shows only when inlined next to live FP value: LTO clang build (`b_lto=true` default) inlined `vmaf_init()` -> `vmaf_init_cpu()` into `test_ciede_device_math`, `45 - 20 * log10(x)` came out `45` on AVX-512 host. Non-LTO: call boundary hides it.
- New inline asm: list every written register in narrowest form too (`xmmN` for `ymmN` / `zmmN`), or compile function for feature.
- Do not move statement back into `cpu.cpp`: `test_cpu` needs it inlinable without LTO.
- gates: `python3 scripts/ci/run_meson_test.py -- -C build test_cpu test_inline_asm_clobber_contract`; clang check needs AVX-512 host (`test_avx512_warm_up_keeps_xmm0` returns early without).
