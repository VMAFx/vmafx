- **`cambi_cuda` runs entirely on the device (ADR-1379).** The CUDA CAMBI twin
  no longer downloads the distorted picture, preprocesses it on the host or
  reads the image and mask back at each of the five scales for host c-values
  and pooling: every stage runs on the GPU, the twin reads the plane the CUDA
  engine already uploaded, and each frame reads back 88 bytes and waits once,
  in `collect()`. Scores equal `--backend cpu` to the last bit whenever the
  CPU's own double top-K sum is exact, and otherwise differ only by that sum's
  rounding. Like `cambi.c`, the twin now rejects an adjusted window above
  65 x 65 ("cambi: window_size N too large for reciprocal LUT") instead of
  reading past the reciprocal table. On an RTX 4090 every frame of the Netflix
  576x324 pair and of BBB 3840x2160 equals `--backend cpu`, and a 4K frame
  takes 6.01 ms instead of 64.71 ms. The old twin also crashed on wide, short
  frames such as 1920x128, where the new one matches the fixed CPU extractor.
  See [CAMBI](docs/metrics/cambi.md#cuda).
