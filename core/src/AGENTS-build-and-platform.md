<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Build, inline asm, and Windows shims: `core/src/` agent invariants

meson.build, x86/avx512_warm_up.h, cpu.cpp, compat/win32/pthread*, MSVC static
library names, and exported-symbol checks.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `meson.build`, `/core/test/check_exported_symbols.py` | [build-and-compiler](AGENTS.d/build-and-compiler.md) | Windows nvcc host = build MSVC, else newest vswhere toolset, else PATH; C++ targets take vmaf_cppflags_common. |
| `x86/avx512_warm_up.h`, `cpu.cpp`, `/core/test/test_cpu.c`, `/core/test/test_inline_asm_clobber_contract.py` | [inline-asm-clobbers](AGENTS.d/inline-asm-clobbers.md) | Inline asm that clobbers ymmN / zmmN also names xmmN; clang drops wide clobber without target feature. |
| `meson.build`, `/scripts/ci/check_msvc_library_names.py` | [msvc-library-names](AGENTS.d/msvc-library-names.md) | Static MSVC-like builds install vmaf.lib / vmafx.lib via vmaf_static_name_kwargs; other builds keep Meson names. |
| `compat/win32/pthread.h`, `compat/win32/pthread_timeout.h`, `/core/test/test_win32_pthread_timeout.c`, `/core/test/test_win32_pthread_shim_contract.py` | [win32-pthread-shim](AGENTS.d/win32-pthread-shim.md) | MSVC threads = header-only Win32 shim, no pthread-win32; timed wait ETIMEDOUT only past deadline. |
