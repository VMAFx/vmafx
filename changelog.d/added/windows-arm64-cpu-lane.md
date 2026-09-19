## Added

- **`Windows ARM64 MSVC` CI lane** (ADR-1260): a native Windows-on-ARM64
  build of libvmaf with the ARM64-hosted MSVC toolset on
  `windows-11-vs2026-arm`, a PE-header check that `vmaf.exe` is an ARM64
  image, and the meson `fast` unit suite executed on ARM64 silicon. It is
  the first lane to compile `core/src/feature/arm64/` and every
  `#if ARCH_AARCH64` branch with `cl.exe`. Advisory (not a required
  check). CPU only until the CUDA 13.4 bump lands. Alongside it:
  `core/test/test_ciede_neon.c`'s guard-page over-read probe runs on
  Windows (`VirtualAlloc` + `PAGE_NOACCESS` + SEH) as well as POSIX, and
  `core/src/meson.build` passes `/fp:precise` instead of
  `-ffp-contract=off` to MSVC for the float NEON carve-outs and skips the
  SVE2 probe there. `docs/getting-started/building-on-windows.md` documents
  the ARM64 recipe.
