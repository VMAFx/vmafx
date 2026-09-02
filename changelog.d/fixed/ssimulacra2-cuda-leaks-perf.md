- **`ssimulacra2_cuda` GPU module leak + per-scale `malloc` removed
  ([ADR-0356](docs/adr/0356-ssimulacra2-cuda-leaks-perf.md)).**
  `init_fex_cuda` calls `cuModuleLoadData` twice (the
  `ssimulacra2_blur` and `ssimulacra2_mul` fatbins) and
  `close_fex_cuda` never called `cuModuleUnload`, leaking
  ~200-500 KB of GPU-resident module backing store per `vmaf_close()`
  cycle. The leak was invisible to `compute-sanitizer --tool memcheck`
  because the leak-checker tracks `cuMem*Alloc` only. Long-running
  pipelines that reinitialise libvmaf per shot (ffmpeg per-segment
  invocation, batch CI runs, the MCP server's stateless mode) would
  accumulate hundreds of MB of GPU memory before host-process exit.
  Fix: guarded `cuModuleUnload` calls in `close_fex_cuda` for both
  module handles. The same fix also removes a per-scale
  `malloc(3 * width * height * sizeof(float))` from the hot path
  (24 MB at 1080p × up to 5 scales / frame, replaced by two
  pre-allocated pinned scratch buffers reused across scales),
  shrinks the H2D / D2H byte count to the valid sub-region per scale
  (~15× PCIe traffic reduction at scale 2 of 1080p: 518 KB valid
  data per plane vs 8 MB full-plane transfer), and adds
  `__launch_bounds__(64, 32)` to `ssimulacra2_blur_h` /
  `ssimulacra2_blur_v` so `nvcc` trims registers to keep ≥32
  resident blocks per SM. Bit-exact at places=4 (0/48 mismatches,
  max abs diff 0.000000e+00) on the Netflix 576×324 fixture via
  `python3 scripts/ci/cross_backend_vif_diff.py … --feature ssimulacra2 --backend cuda`.
  The H-pass non-coalesced reads and V-pass L1 pressure ceilings
  remain known follow-ups (require a shared-memory tile-transpose
  rewrite). The `cuModuleUnload` rule is propagated to
  `core/src/cuda/AGENTS.md § Lifecycle invariants` so future
  agent passes pin it on every CUDA extractor; every existing
  extractor leaks the same way (separate `T-CUDA-MODULE-UNLOAD-SWEEP`
  PR).
