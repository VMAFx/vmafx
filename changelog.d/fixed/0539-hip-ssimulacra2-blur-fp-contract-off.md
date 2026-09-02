- HIP backend: `ssimulacra2` parity vs CPU now passes the places=2
  cross-backend tolerance gate. The fork's HIP HSACO build pipeline
  was compiling `ssimulacra2_blur` with hipcc's default
  `-ffp-contract=fast`, which silently fused the recursive Gaussian
  IIR step (`n2 * sum - d1 * prev`) into FMAs and shifted the pole
  cascade past places=2 vs the CPU reference within a handful of
  pyramid levels. The CUDA twin already disables this via
  `cuda_cu_extra_flags : ['--fmad=false']`; the HIP scaffolding had
  no equivalent dispatch. Adds a `hip_cu_extra_flags` dict in
  `core/src/meson.build` mirroring the CUDA pattern, with first
  entry `'ssimulacra2_blur' : ['-ffp-contract=off']`. Other kernels
  fall through to byte-identical command lines. Verified on the
  iGPU (gfx1036) Netflix golden pair — `--backend hip --feature
  ssimulacra2` now matches CPU to display precision (6 decimal
  places). See ADR-0539.
