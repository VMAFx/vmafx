- **`scripts/dev/hip_dispatch_drop_probe.hip` checks whether an AMD GPU runs
  every command of a HIP stream.** Built with `hipcc`, it runs frames of one
  memset, several small kernels and a readback on one stream and reports the
  frames with wrong results and the kernel launches that never ran. On the
  maintainers' gfx1036 iGPU (ROCm 7.2.4) about one frame in 10^4 loses a run
  of its commands, which makes a HIP twin report a wrong score for that frame
  on master as well; the probe tells whether a driver update fixed it. See
  [the HIP backend guide](docs/backends/hip/overview.md#known-issue-the-gfx1036-loses-stream-commands)
  (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`).
