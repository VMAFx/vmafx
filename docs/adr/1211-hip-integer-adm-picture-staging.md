<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1211: `integer_adm_hip` stages the luma plane onto the device before launching

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: hip, correctness, feature-extractor, adm

## Context

`integer_adm_hip` faulted the GPU on the first frame and killed the process:

```text
Memory access fault by GPU node-1 on address 0x556905b5a000.
Reason: Page not present or supervisor privilege.
```

Tracked as `T-HIP-INTEGER-ADM-GPU-PAGE-FAULT-2026-09-05` and reproduced here on
a gfx1030. Running with `AMD_SERIALIZE_KERNEL=3 HIP_LAUNCH_BLOCKING=1
AMD_LOG_LEVEL=3` named the offender as the very first kernel launched,
`adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t`, and the faulting
address is in the host heap range — the tell that a **host** pointer reached a
device kernel.

It did. `extract_fex_hip` passed `ref_pic->data[0]` and `dis_pic->data[0]`
straight into `dwt2_8_device_hip` / `dwt2_16_device_hip`. Under the HIP backend
`VmafPicture::data[]` is host memory: the HIP backend is *host-pic*
(ADR-0530) and, unlike CUDA, gets no device picture from a pool. The CUDA twin
needs no staging because `vmaf_cuda_picture_*` hands it device memory already;
the HIP port inherited the CUDA call shape without the thing that made it
valid. `integer_psnr_hip` already does the staging correctly with its
`ref_in` / `dis_in` buffers, which is why `test_hip_psnr_parity` passed while
the ADM test cored.

This is the second half of `T-GAP-HIP-INTEGER-ADM-PICTURE-STAGING-DEFERRED-2026-09-02`.

## Decision

We will allocate a per-side device staging buffer for the scale-0 luma plane in
`init_fex_hip` and `hipMemcpy2DAsync` the host plane into it at the top of
extract, then pass the device pointers to the DWT2 kernels. The staged rows are
tightly packed, so the element stride handed to the kernel becomes `w` rather
than the picture's stride.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-extractor device staging buffers, mirroring `integer_psnr_hip` (chosen) | Matches the pattern already proven on this backend; local to the broken extractor; no change to the host-pic contract | Each HIP extractor that needs device input pays for its own staging buffer | — |
| Make the HIP backend device-pic like CUDA | One staging point for every HIP extractor | Changes ADR-0530's backend contract and touches every HIP extractor and the picture pool — a much larger blast radius for a crash fix | Rejected for now; worth revisiting if more HIP extractors need device input |
| Register `integer_adm_hip` with `.flags = 0` so it is never selected | Trivially stops the crash | Removes a user-facing extractor rather than fixing it, and the CPU fallback already hid the defect from CI | Rejected |
| Use managed / host-registered memory (`hipHostRegister`) | No explicit copy | Pins caller memory the extractor does not own, and performance is worse on discrete GPUs | Rejected |

## Consequences

- **Positive**: `test_hip_adm_parity` goes from a GPU coredump to **3/3 passing**,
  and the HIP parity suite goes 16 pass / 2 fail to **17 pass / 1 fail** on a
  gfx1030. `vmaf --backend hip --feature adm_hip` returns real scores
  (`integer_adm2 = 0.962084`) instead of killing the process.
- **Negative**: one extra host-to-device copy of the luma plane per frame, and
  two device allocations of `w * h * bytes-per-sample`. Unavoidable while the
  backend is host-pic.
- **Neutral / follow-ups**: the copy is followed by a `hipStreamSynchronize`
  for correctness on the first cut. Overlapping it with the existing
  ref/dis events is a performance follow-up, not a correctness one. The one
  remaining HIP parity failure, `test_hip_ssim_parity` (4.53e-03), is the
  separately-tracked `T-GAP-HIP-INTEGER-SSIM-FLOAT-KERNEL-DEFERRED-2026-09-02`
  — an 11-tap float Gaussian where the CPU uses a 9-tap int64 kernel, formally
  deferred under ADR-0564.

## References

- `T-HIP-INTEGER-ADM-GPU-PAGE-FAULT-2026-09-05` and
  `T-GAP-HIP-INTEGER-ADM-PICTURE-STAGING-DEFERRED-2026-09-02` in
  [docs/state.md](../state.md).
- The working pattern: `core/src/feature/hip/integer_psnr_hip.c` (`ref_in` /
  `dis_in` + `hipMemcpy2DAsync`).
- [ADR-1154](1154-hip-backend-gaps.md) — the HIP backend gap inventory.
- Reproducer: `meson test -C build test_hip_adm_parity` on a ROCm device; before
  this change the process dies with the fault above.
- Source: `req` — user direction to fix bugs.
