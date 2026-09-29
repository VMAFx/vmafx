- **`--feature <name>` now runs on the GPU that `--backend` names.**
  `vmaf --backend sycl --feature ciede` used to initialise the SYCL device and
  compute `ciede2000` with the CPU extractor, one frame at a time, while the
  JSON reported `"backend_used": "sycl"`. With an explicit `--backend cuda`,
  `sycl`, `hip` or `metal`, a CPU extractor name now runs on that backend's
  twin (`ciede_sycl` here), chosen the way a model's features are
  ([ADR-1359](docs/adr/1359-cli-feature-backend-twin.md)). When the backend has
  no twin, or the twin cannot honour an option or the input size, the CPU
  extractor runs and one `vmaf: warning: --feature <name>: ...` line says why.
  Twin names such as `--feature ciede_sycl`, `--backend cpu`, `--backend auto`
  and runs without `--backend` behave as before.
- **`backend_used` reports the backend that computed the features.** It used to
  name the backend that was initialised, even when nothing ran on it. It now
  names the device when at least one extractor ran there and `cpu` otherwise;
  the new `feature_backends` array says where each extractor ran.
