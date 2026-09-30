- **`ssimulacra2_cuda` runs entirely on the device (ADR-1391).** The CUDA
  ssimulacra2 twin no longer copies the pictures to the host, converts colour,
  computes XYB, combines the SSIM and edge-difference maps or downsamples on the
  host, and no longer copies five buffers back and waits at every scale: each
  frame is one chain of kernels on the picture stream and one 864-byte readback.
  On an RTX 4090 a 3840x2160 frame takes about 7 ms instead of about 720 ms, a
  1920x1080 frame about 2 ms instead of 239, and a 576x324 frame under 1 ms
  instead of about 17 (the CPU extractor on 16 threads: about 170, 33 and
  2 ms).
  Its score is within about 1e-12 of `--backend cpu` (1.5e-12 at worst on the
  tested content, the same on every run) where it used to match bit for bit:
  the per-pixel terms are the CPU's double-precision expressions, added in a
  fixed tree instead of one after another. 4:0:0 input and frames below 8x8 now
  go to the CPU extractor. See [SSIMULACRA 2](docs/metrics/ssimulacra2.md).
