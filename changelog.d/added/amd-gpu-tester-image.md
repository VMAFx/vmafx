- **An AMD GPU tester image measures every HIP twin on an outside tester's AMD
  GPU with one command and no build.**
  `ghcr.io/vmafx/vmafx:<version>-tester-hip` (linux/amd64) runs on Linux with
  `--device /dev/kfd --device /dev/dri` and the render group. It finds every AMD
  GPU the image has code for (Instinct MI100 to MI350, Radeon RX 6000, 7000 and
  9000 series, Radeon 680M, 780M, 890M and 8060S graphics) and reports per GPU its
  gfx target and family, every HIP twin against the CPU at `--precision max`, the
  parity gate's HIP cells, the HIP device tests, and the verdict of
  `T-HIP-TWINS-OTHER-TARGETS-2026-10-03` for its family. It ships only the ROCm
  10.0.0 runtime files the HIP build loads, unmodified, with their licences and the
  source of the LGPL libraries among them. See
  [the tester guide](docs/usage/tester-image.md#e-amd-gpu-image-linux)
  and [ADR-1511](docs/adr/1511-amd-gpu-tester-image.md).
