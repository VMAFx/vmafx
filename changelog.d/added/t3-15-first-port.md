- **CUDA `psnr` chroma extension** (`psnr_cuda` now emits `psnr_y`,
  `psnr_cb`, `psnr_cr`) — first port of T3-15(a) GPU coverage long-tail
  batch 4. Mirrors the Vulkan twin shipped in PR #204 / ADR-0216 and
  the CPU contract at `core/src/feature/integer_psnr.c`. Cross-backend
  `cross_backend_vif_diff.py --feature psnr --backend cuda --places 4`
  clears bit-exactly (0/48 mismatches on the Netflix normal pair, all
  three planes). Companion research digest at
  [`docs/research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md`](docs/research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md)
  enumerates the remaining seven gaps (SYCL PSNR chroma, CUDA + SYCL
  chroma SSIM / MS-SSIM, CUDA + SYCL `cambi`) for follow-up PRs.
