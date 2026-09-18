- **The HIP integer ADM kernels read their buffer description through a
  pointer again (ADR-0759).** The change listed as "HIP ADM: AdmBufferHip
  passed by pointer" had been undone by a later merge, so four kernels were
  still copying the 328-byte struct into their arguments on every launch. It
  is back: each launch now passes one device pointer. Scores are
  byte-identical and 1080p throughput is unchanged within run-to-run noise.
