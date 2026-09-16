
- **The SYCL backend could not score a single frame.** `--frame_cnt 1` exited
  255 with "problem generating pooled VMAF score" and wrote no JSON at all;
  two or more frames were fine. `collect()` writes `motion2[0] = 0` for the
  first frame but back-fills `motion3[0]` only when a *second* frame arrives,
  and `flush()` emitted the delayed scores only when more than one frame had
  been seen — so a one-frame run produced no `motion3` and the model had
  nothing to predict from. The CPU twin's flush emits both for every frame,
  including index 0. `flush_fex_sycl` now does the same, and a single-frame
  SYCL run scores 89.4663986 against the CPU's 89.4663965, the same ~2e-6
  agreement it has at any other frame count.
- **SpEED chroma SYCL/CPU parity on the U channel is 36x tighter.** The CPU
  reference accumulates the covariance in `double`, so every float product is
  exact; the SYCL kernel summed ~45,000 terms in `float`. The device has no
  fp64 at all (`aspect::fp64` is false on Arc A380 and a double kernel is
  rejected), so the sum is now carried as a compensated (hi, lo) float pair
  with each product split exactly by one FMA — verified fused on-device. The U
  channel went from 1.37e-4 to 3.8e-6 against a 1e-4 tolerance.
- **The SYCL back-substitution disagreed with the CPU about singularity.** It
  treated a pivot as singular below `1e-8f` and zeroed just that row, while
  `speed_internal_backward_substitution` uses `1e-6f` and returns `-EINVAL`,
  which `est_params` folds into the same path a non-regular covariance takes.
  A pivot between the two thresholds produced a solution the CPU never
  computes. The epsilon is now shared from `speed_internal.h` so the GPU twins
  cannot drift from it, and the host checks the R diagonal before committing to
  the device solve. The score kernel also divides each term before summing, as
  `compute_pointwise_product_and_division` does, rather than summing first.
