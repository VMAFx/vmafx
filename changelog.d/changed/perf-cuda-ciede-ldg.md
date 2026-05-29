### CUDA CIEDE2000 — `__ldg()` read-only cache routing (ADR-0762)

Apply F3 fix (mirror of ADR-0754 / PR #93 SSIM pattern) to
`calculate_ciede_kernel_8bpc` and `calculate_ciede_kernel_16bpc`:

- Extract `const uint8_t *__restrict__` (8bpc) and `const uint16_t *__restrict__` (16bpc)
  channel pointers from `VmafPicture` struct args before the per-pixel body.
- Replace all 6 indexed channel reads with `__ldg(&ptr[idx])` to route loads through
  the L1 read-only texture cache (L2-pressure reduction at 1080p and above).
- Add `__launch_bounds__(BLOCK_X * BLOCK_Y)` register-budget hint to both kernels.
- CUDA vs CPU correctness: places=4 PASS, max diff = 0.0 on Netflix 576×324 reference pair.
