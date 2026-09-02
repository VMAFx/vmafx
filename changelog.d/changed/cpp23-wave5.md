### C++23 Wave 5: cpu, ref, thread_locale

`core/src/cpu.c`, `ref.c`, and `thread_locale.c` are converted to `.cpp`
with C++23 idioms (ADR-0735).

- `cpu.cpp`: global CPU-flag state converted from bare `static unsigned`
  to `std::atomic<unsigned>` — eliminates a potential data-race during
  multi-threaded initialisation; `memory_order_relaxed` preserves zero
  overhead on the hot `vmaf_get_cpu_flags()` read path.
- `ref.cpp`: `vmaf_ref_init` uses `std::make_unique<VmafRef>()` (replaces
  `malloc`+`memset`+`atomic_init`); early-return error paths are leak-safe;
  `vmaf_ref_close` uses `delete` (matching the `operator new` allocator).
- `thread_locale.cpp`: platform teardown (`uselocale`/`freelocale` on POSIX,
  `_configthreadlocale` on Windows) encapsulated in `VmafThreadLocaleState`'s
  destructor; `vmaf_thread_locale_pop` reduced to a `unique_ptr` adopt-and-
  destroy one-liner; `std::array<char, 256>` replaces C `char[256]` on
  Windows.

Public C ABI preserved unchanged. Each file compiled as an isolated
`static_library` with `cpp_std=c++23`.
