- `cambi_sycl`'s `launch_reset` kernel now uses an explicit 1D `nd_range` and a
  scalar select chain for its per-scale top-K rank initialization instead of an
  array captured by value and indexed at runtime in the kernel closure
  (ADR-1395). This eliminates 1280 B of private array scratch memory and the
  896 B `RoundedRangeKernel` wrapper on Intel GPUs under the Linux xe driver,
  leaving every kernel in `integer_cambi_sycl.cpp` completely scratch- and
  spill-free. Bit-identical parity against `--backend cpu` is preserved on all
  tested fixtures (48/48 frames on Netflix 576x324, 50/50 frames on BBB 4K, max
  abs diff 0.0), with 4K throughput measured at 17.05 ms/frame on Arc A380
  (down from 18.53 ms/frame).
