- `scripts/test/repro-cuda-ffmpeg-nondeterminism.sh` reproduces
  `T-CUDA-FFMPEG-FILTER-NONDETERMINISM-2026-09-06` — the FFmpeg `libvmaf_cuda` filter
  intermittently returning a wrong pooled VMAF — and reports which frames and metrics
  corrupted. The defect is timing-dependent: it does not reproduce on an idle host and
  reaches ~23% under load, so the script prints the load average alongside its result and
  documents why two builds must be compared by interleaving runs rather than sequentially.
