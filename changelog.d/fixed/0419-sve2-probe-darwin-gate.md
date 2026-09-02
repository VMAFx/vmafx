- macOS-arm contributor build no longer fails with *"SVE vector
  type 'svbool_t' cannot be used in a target without sve"*
  (ADR-0419). Apple Silicon (M1–M4) is ARMv8.x without SVE2 and
  the runtime detection in `core/src/arm/cpu.c` has always
  been `__linux__`-gated, but recent Apple Clang accepts
  `-march=armv9-a+sve2` against the declarations-only probe TU
  so `cc.compiles()` returned true on Apple Silicon while the
  real SSIMULACRA 2 SVE2 TU failed to compile under Apple's
  incomplete intrinsics surface. GHA `macos-latest` (Apple
  Silicon since late 2024) did not catch this because its image
  Apple Clang version makes the probe fail outright; only
  newer local Xcodes trip the inconsistency. Fixed by
  short-circuiting `is_sve2_supported = false` on
  `host_machine.system() == 'darwin'` in
  `core/src/meson.build`, mirroring the runtime gate. Linux
  ARMv9 (Graviton 4, Ampere AmpereOne) builds unaffected.
