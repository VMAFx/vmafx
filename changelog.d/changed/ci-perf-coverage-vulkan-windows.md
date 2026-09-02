- **CI:** Per-lane wall-clock optimizations for the three slowest CI lanes
  identified in Research-0089 (PR #525) §2: (1) Coverage Gate caches the
  ONNX Runtime GPU `.tgz` (~150 MB) keyed on the pinned ORT version, saving
  ~30–60 s per run; (2) Ubuntu Vulkan build caches `core/subprojects/packagecache/`
  so volk + VMA wrap archives are restored from the GHA cache instead of
  re-fetched from GitHub releases on every run, saving ~15–30 s per run;
  (3) Windows MSVC + CUDA enables `use-github-cache: true` on the
  `Jimver/cuda-toolkit` action so the CUDA 13.0.0 installer payload (~3 GB)
  is restored from the GHA cache instead of re-downloaded over the network,
  saving ~2–4 min per run. No coverage change. See
  `docs/research/0089-ci-cost-optimization-audit-2026-05-09.md`.
