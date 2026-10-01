- **The first-release candidate plan now has eight candidates.**
  `v1.0.0-rc.3` owns twin exactness (every GPU and SIMD twin returns the CPU
  extractor's scores bit for bit, or carries a measured tolerance), `rc.4` the
  first full Rust metric (the whole `vmaf_v1.0.16_3d0h` path), `rc.5`
  deduplication, `rc.6` a generated per-vendor GPU capability table with a
  static audit of every kernel for every target, `rc.7` benchmarks, profiling
  and tuning, and `rc.8` the one-shot model retrain. Benchmarks and the retrain
  move back so that they run on a tree that is no longer being corrected or
  restructured. The release guide, roadmap, retrain runbook, tester guide,
  model card and `vmaf-rc1-report list-tools` inventory show the new mapping
  (ADR-1421).
