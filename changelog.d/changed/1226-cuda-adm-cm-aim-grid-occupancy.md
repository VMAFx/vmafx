- **CUDA ADM is 19-31% faster, with bit-identical output** (ADR-1226). The AIM
  contrast-masking kernel launched one block per 32 rows of the band, which on
  a 1080p frame is 42 blocks against an RTX 4090's 128 SMs — most of the device
  idle by construction. The launch now picks its `rows_per_thread` from the
  device's SM count (4, or 2 on frames too small to fill half of them), which
  changes only how rows are distributed across threads: each row is still
  reduced across all its columns inside one block, so the emitted score does
  not move. Measured with `vmaf_bench --gpu-only` on an RTX 4090, mean ms per
  frame for the whole `adm (CUDA)` feature: 1920x1080 1.31 → 1.06, 640x480
  0.46 → 0.33, 576x324 0.39 → 0.27. `vmaf_bench --validate` reports identical
  CPU-vs-CUDA `max_diff` values before and after.
