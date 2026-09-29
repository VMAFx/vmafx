- **The SYCL SpEED twins run entirely on the device and match the CPU bit for
  bit (ADR-1358).** `speed_chroma_sycl` and `speed_temporal_sycl` no longer
  filter, factorise the 25x25 covariance or wait on the queue on the host
  between device passes: each frame is one upload, one replayed SYCL graph and
  one result read. On an Arc B580, `speed_chroma` at 3840x2160 drops from 23.3
  to 7.5 ms per frame and `speed_temporal` from 60.4 to 7.6 (CPU with 16
  threads: 7.2 and 37.3); at 576x324 both run in under a millisecond. Every
  per-frame `speed_chroma_u/v/uv` and `speed_temporal` score now equals
  `--backend cpu` exactly, where previously most frames differed by up to
  4.2e-5. Request the twins by name (`--feature speed_chroma_sycl`):
  `--feature speed_chroma --backend sycl` runs the CPU extractor.
  `scripts/dev/speed_gpu_parity.py` checks and times any GPU twin against the
  CPU. See [SpEED](docs/metrics/speed_qa.md).
