- The K150K/CHUG feature extractor now routes `float_ssim` through
  the CUDA primary pass (with explicit `scale=1` per the
  `float_ssim_cuda` v1 contract) instead of the CPU residual pass.
  Per-clip wall time on CUDA workers improves by roughly the CPU
  SSIM cost. `cambi` was originally also planned for promotion;
  the `cambi_cuda` SIGSEGV that blocked promotion (Issue #857) has
  since been fixed (PR #866 + PR #870, 2026-05-16), but the K150K
  script intentionally still routes `cambi` through the CPU
  residual pass — a follow-up will re-evaluate promotion against a
  rebuilt CUDA binary.
