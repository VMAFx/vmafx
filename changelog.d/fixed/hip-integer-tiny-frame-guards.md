- **HIP twins stay inside their buffers on small frames, and `vif_hip` hands
  frames below 16 pixels to the CPU (ADR-1381).** The HIP motion kernel's tile
  loads and the integer ADM scale-0 vertical DWT reflect an index once, which
  leaves the plane for the padding threads of a plane smaller than the tile
  (the defect that faulted the SYCL twins); both now clamp the reflected row
  into the plane, which changes no score of any accepted frame.
  `float_motion_hip` had the same single reflection and read before its input
  plane on 3x3 to 9x9 and 17x17 frames; its tile loads clamp the same way.
  `vif_hip` scored frames below 16 pixels from other samples than the CPU (its filters
  need 16 pixels at every scale); model dispatch now computes those frames
  with the CPU `vif`, and `--feature vif_hip` below 16x16 fails at init. The
  device tests pass on a gfx1036 with no GPU memory fault; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).
