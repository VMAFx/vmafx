- **Per-backend performance baseline harness measures the 16 newest models.**
  `testdata/bench_backends.py` supports all 16 newest built-in models
  (8 SDR `vmaf_v1.0.16*` and 8 HDR `hdr_vmaf_v1.1.8*`), adds colorimetry and
  pixel format definitions for PQ fixtures (`sollevante_pq_1080p`), enforces
  model-to-fixture compatibility (HDR models on SDR fixtures strictly
  refused), and validates backend engagement from `backend_used` and
  `feature_backends` to prevent silent fallback. Per-backend baseline throughput
  is published in [`docs/benchmarks.md`](docs/benchmarks.md#newest-models-2026-10-31-commit-bdd7f9c70).
