# Research 0130: CUDA psnr_hvs DCT parallelisation

## Scope

Close the remaining T-GPU-OPT-3 kernel-side item for `psnr_hvs_cuda`.
The host drain-batch path is already in tree; the remaining hot-path
gap is that the CUDA kernel still ran the entire 8x8 DCT on thread 0.

## Findings

- `integer_psnr_hvs_cuda.c` already enqueues partial readback during
  submit and waits through `vmaf_cuda_kernel_collect_wait()` in collect,
  so this PR does not touch the host-side drain-batch lifecycle.
- `psnr_hvs_score.cu` loaded each 8x8 block cooperatively but returned
  every thread except thread 0 before the DCT, masking, and reductions.
- The DCT itself is integer-only and consists of independent 8-point
  transforms over eight columns followed by eight transforms over the
  intermediate rows.
- The numeric risk is not the integer DCT; it is the float means,
  variances, masking, and final masked-error accumulation. Those remain
  thread-0 serial in the same CPU scan order.

## Decision

Run the two integer DCT passes across the first eight CUDA threads
inside each block, using shared memory for the intermediate transpose.
Keep all float accumulation serial on thread 0.

## Alternatives

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| Parallelise only the integer DCT | Removes the obvious thread-0-only DCT bottleneck while preserving float reduction order. | Leaves some scalar work in the kernel. | Chosen. |
| Parallelise DCT and float reductions | Larger speedup potential. | Changes IEEE-754 summation order and risks drifting the established places=3 CUDA/Vulkan contract. | Rejected for this PR. |
| Leave the kernel serial and rely on host drain-batch only | Zero numeric risk. | Leaves the explicit T-GPU-OPT-3 kernel-side item open. | Rejected. |

## References

- User request: "so you want to tell me that all we have left is docs? ... no way"
- `.workingdir2/BACKLOG.md` T-GPU-OPT-3
- `docs/rebase-notes.md` CUDA psnr_hvs drain-batch invariant
