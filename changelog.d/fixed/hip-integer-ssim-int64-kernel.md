- **`integer_ssim_hip` now computes the CPU's integer SSIM, and model-driven
  `ssim` runs on the HIP backend** (ADR-0564). The HIP kernel was an 11-tap
  float Gaussian where the CPU `ssim` extractor uses a 9-tap int64 kernel, so
  it scored 4.53e-3 away from the CPU and was left out of HIP dispatch: under
  `--backend hip` a model requesting `ssim` quietly ran it on the CPU. The
  kernel is now a port of the CUDA twin, with int64 moments, the CPU's window
  truncation at the frame border, and the per-pixel term evaluated exactly as
  the CPU evaluates it. Against the scalar CPU the worst per-frame delta over
  8-, 10-, 12- and 16-bit inputs from 1x1 to 1920x1080 is 1.06e-11, the same as
  the CUDA twin. `--feature integer_ssim_hip` also accepts any frame size now;
  it used to reject frames smaller than 11x11.
- **`integer_ssim_hip` no longer scores frames against the next frame's
  samples.** It uploaded each host picture asynchronously and returned while
  the copy could still be reading, and the CLI's picture pool refilled that
  buffer with the next frame. On a multi-frame run a different set of frames
  came out wrong on every run (by up to 0.2 on the Netflix 576x324 pair). The
  extractor now waits for its uploads. Other HIP extractors that upload the
  same way are affected too and are tracked in `docs/state.md`
  (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
