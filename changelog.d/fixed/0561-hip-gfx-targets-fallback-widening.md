**fix(hip): widen hardcoded `gfx_targets` fallback to `gfx90a,gfx1030,gfx1036,gfx1100` (ADR-0561)**

The HIP build's no-GPU-probe fallback was `gfx90a` only (CDNA2 server). Any
`libvmaf.so` compiled in a build sandbox (BuildKit, CI) contained no HSACO
blob for RDNA2/RDNA3 consumer GPUs, causing a runtime `No compatible code
objects found for: gfx1030` failure on the fork's primary dev host (AMD Raphael
APU `gfx1036`, override-mapped to `gfx1030`). The fallback now covers
`gfx90a` (CDNA2), `gfx1030` (RDNA2 desktop + Raphael override), `gfx1036`
(Raphael iGPU native), and `gfx1100` (RDNA3 desktop). Builds where
`rocm_agent_enumerator` or `hipconfig` succeeds are unaffected.

Also documents the four-step target resolution order in
`docs/backends/hip/overview.md`.
