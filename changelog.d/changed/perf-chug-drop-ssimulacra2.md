perf(ai): drop ssimulacra2 from K150K/CHUG self-vs-self extraction

ssimulacra2 produces a constant ~100 in identity pairs (FR-from-NR adapter where
ref == distorted), yielding zero training signal while consuming 30-50% of GPU
time per clip. Parquet schema v1 → v2: 22 features → 21 (ssimulacra2 removed).
Wall-time improvement: ~30-50% per clip in CUDA-enabled extraction mode.
See Research-0431 and ADR-0431 for decision matrix and alternatives considered.
