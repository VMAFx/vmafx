- **SYCL zero-copy scores no longer drift from run to run under batched
  command lists.** With `UR_L0_USE_IMMEDIATE_COMMANDLISTS=0`, the setting the
  SYCL bundling page recommends for Arc A-series GPUs, FFmpeg's `libvmaf_sycl`
  on QSV-decoded input stopped importing new frames from a random frame on in
  3 to 7 of every 10 runs on an Arc A380 (compute-runtime 26.35). The driver
  silently dropped the per-frame surface import once each frame's DMA-BUF import
  was mapped at the GPU address the previous frame's import had just freed, so
  every extractor scored two stale frames; `cambi` showed it as a frozen pair
  of values and differed from host upload by up to 2.3. libvmaf now creates its
  primary SYCL queue, which runs the import, with immediate command lists
  whatever the variable says; the other queues still follow it. Zero-copy
  `cambi`, `vif` and `vmaf_v0.6.1` now equal host upload in 10 of 10 runs on
  the Netflix pair and the 1080p checkerboard at 8 and 10 bit, at unchanged
  1080p throughput. `scripts/test/zerocopy-e2e.sh --repeat N` repeats the
  zero-copy leg and fails on any differing run (ADR-3359,
  `T-SYCL-ZEROCOPY-IMPORT-DROPPED-2026-10-02`;
  [SYCL bundling](docs/backends/sycl/bundling.md),
  [SYCL zero-copy testing](docs/development/sycl-zerocopy-testing.md)).
