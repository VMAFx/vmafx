- **`motion_sycl` matches the CPU `motion` exactly.** The SYCL twin blurred
  each frame and differenced the blurred frames, while the CPU (since the
  upstream pipelined-motion port) blurs the frame difference and rounds after
  each filter pass. The two orders round differently, so `motion2` was up to
  2.0e-4 off on 17x17 frames, 1.3e-5 on the Netflix 576x324 pair and 5.6e-6
  at 4K. `motion_sycl` and `motion_v2_sycl` now run one kernel with the CPU's
  arithmetic and agree with the CPU bit for bit at every size and bit depth
  tested, on an Arc B580 and a UHD 770 (ADR-1371). The 4K motion step costs
  about 11% more device time on both GPUs. With `motion_add_uv=true`,
  `motion_sycl` no longer waits on the device inside `submit()`: the U and V
  planes are staged in pinned memory and uploaded on the compute queue, which
  cuts host time per 4K frame from 5.4 to 0.6 ms on a UHD 770
  ([SYCL backend](docs/backends/sycl/overview.md#motion_sycl-matches-the-cpu-motion-exactly-2026-09-29)).
