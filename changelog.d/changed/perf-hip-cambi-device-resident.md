- **`cambi_hip` runs every CAMBI stage on the device (ADR-1378).** The HIP
  twin no longer preprocesses the picture on the host or copies the image and
  mask back at every scale to compute the c-values and the top-K pooling
  there: each frame is one staged upload of the luma, the whole
  ADR-1357 pipeline on the extractor's stream and one 88-byte read of the
  exact per-scale sums, with `collect()` the only wait. Scores are expected
  to equal `--backend cpu` bit for bit wherever the CPU's own top-K sum is
  exact. `cambi_hip` now also refuses, as the CPU extractor does, a window
  whose adjusted size exceeds 65 x 65 ("cambi: window_size N too large for
  reciprocal LUT"). Not yet run on an AMD device: the verify and timing
  commands are in `docs/state.md` (`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`)
  and [CAMBI](docs/metrics/cambi.md#hip).
