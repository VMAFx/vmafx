- The Windows MSVC builds no longer print the C runtime, declaration and
  command-line warnings: `strdup`, `close`, `sscanf`, `getenv`, `_wfopen`,
  `_wopen` and `_open` are called through the CRT's own non-deprecated
  spellings (`src/compat/crt_portable.h`, `_wfsopen`, `_wsopen_s`, `_sopen_s`);
  `strncpy` became a bounded `memcpy`; `thread_locale.cpp` declares its state as
  the `struct` the header names (C4099); the pdjson test copies no longer rename
  `push` / `pop` as macros, which broke `#pragma warning(push)` in the system
  headers (C4615, C4079, C4081); unknown `STDC FP_CONTRACT` / `clang fp
  contract` pragmas are not given to `cl.exe` (C4068); the SIMD libraries get the
  `-mavx2` family only from compilers that take it (D9002). Configure `cl.exe`
  builds with `-Dc_std=none` (D9025; `docs/getting-started/install/windows.md`).
