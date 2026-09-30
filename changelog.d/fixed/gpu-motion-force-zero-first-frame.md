- **`motion_force_zero` no longer crashes `motion_cuda` and
  `float_motion_cuda`.** With the option set, the twins' `init()` switches
  them from the asynchronous `submit()` / `collect()` pair to a synchronous
  `extract()` that publishes zeros, but the engine had already chosen the
  asynchronous path and called the cleared `submit()` on the first frame:
  `--backend cuda --feature motion_cuda=motion_force_zero=true` (or
  `float_motion_cuda=...`) died with SIGSEGV. The engine now initialises such
  an extractor before it picks the path, so both twins publish zeros for
  every frame, as the CPU extractors do. The HIP and Metal motion twins make
  the same switch and take the same engine path
  (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`).
