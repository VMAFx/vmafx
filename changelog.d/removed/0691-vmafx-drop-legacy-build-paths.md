- **CI: MinGW64 Windows build removed** — the `Build — Windows MinGW64 (CPU)`
  job (MSYS2 / MinGW-w64 GCC, static link, `vmaf.exe` artifact) is no longer
  present in the CI matrix. Windows coverage is provided by the MSVC + CUDA
  and MSVC + oneAPI SYCL build-only legs. (ADR-0691, VMAFX Phase 1C)
- **CI: i686 / no-asm 32-bit Linux build removed** — the `Build — Ubuntu i686
  gcc (CPU, no-asm)` matrix entry (`--cross-file=build-aux/i686-linux-gnu.ini
  -Denable_asm=false`) is no longer present. The fork targets 64-bit x86-64
  and ARM64 exclusively. (ADR-0691, VMAFX Phase 1C)
