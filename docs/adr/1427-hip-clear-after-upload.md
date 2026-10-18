<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1427: A HIP frame queues its accumulator clears after its upload

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `hip`, `gpu-parity`, `testing`, `rc3`, `fork-local`

## Context

Several HIP twins add into accumulators on the device and clear them every
frame with `hipMemsetAsync` on their stream. Four queued that clear ahead of
the frame's plane upload: `adm_hip`, `float_moment_hip`, `vif_hip` and
`float_psnr_hip`. The others queue it after the upload.

On a gfx1036 (ROCm 7.2.4) a clear queued ahead of the upload has no effect in
the first context of a process that needs larger planes than the contexts
before it. The kernels then add onto what the accumulators hold, and a
recycled device allocation holds the sums of the earlier context. Measured on
`origin/master` e2954fc63 with one frame in a 640x360 context and then one in
a 3840x2160 context, every twin in its own process, three runs each:

| Twin | Clear queued | First frame of the 3840x2160 context |
|---|---|---|
| `float_moment_hip` | ahead of the upload | `float_moment_ref1st` 130.53, CPU 127.00: the frame's sums plus the smaller context's |
| `vif_hip` | ahead of the upload | scale 0 to 2: 0.6748, 0.8040, 0.8749; CPU 0.6934, 0.8260, 0.8988 |
| `adm_hip` | ahead of the upload | `invalid ADM reduction`, the run fails ([ADR-1423](1423-hip-adm-cpu-row-rounding.md) moved its clear) |
| `float_psnr_hip` | ahead of the upload | correct: its kernel writes every partial |
| `psnr_hip`, `float_vif_hip`, `float_adm_hip`, `cambi_hip` | after the upload | correct |
| `ciede_hip`, `integer_ssim_hip`, `float_ssim_hip`, `integer_ms_ssim_hip`, `psnr_hvs_hip`, `ssimulacra2_hip` | no accumulator clear | correct |

The three wrong twins are wrong in every run. The `vmaf` tool has one context
per process, where fresh device memory is zero, so it never showed; a program
that scores a small clip and then a larger one through the library did.

What narrows it down, measured on `float_moment_hip`:

- With the clear queued after the upload the frame is correct in 23 of 23
  runs. Work added between that clear and the kernel does not bring the
  failure back: a 32 MiB `hipMalloc`, a `hipMalloc` and `hipFree`, a second
  upload of the picture, an upload of a new 8 MiB pageable or page-locked
  host buffer into a new device buffer, an event wait (3 of 3 runs each).
- A `hipStreamSynchronize()` after a clear queued ahead of the upload makes
  the frame correct.
- A `hipMemset` when the buffer is allocated does not. On device memory that
  call is not synchronous: hipamd's `ihipMemset()` queues the fill on the
  null stream and returns (`rocm-7.2.4` source), and a non-blocking stream is
  not ordered against the null stream.
- `vif_hip` runs on a blocking stream, `adm_hip` and `float_moment_hip` on
  non-blocking ones. Both kinds fail.
- The same larger context created a second time is correct.

Why the clear is lost is the runtime's or the driver's and is not established.

## Decision

A HIP frame uploads its planes, then queues its clears, then launches its
kernels. No function of a HIP twin queues a clear (`hipMemsetAsync`,
`hipMemset` or a helper that calls one) and uploads afterwards
(`vmaf_hip_plane_source_acquire*()`, `vmaf_hip_picture_upload*()` or a helper
that calls one). A clear at allocation is not a substitute.

`float_moment_hip`, `vif_hip` and `float_psnr_hip` are reordered here;
`adm_hip` in ADR-1423.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Queue the clear after the upload (this ADR) | No cost; the order the correct twins already had; one rule a source check can hold | Rests on a measured order, the cause in the runtime is unknown | Chosen |
| `hipStreamSynchronize()` after every clear | Correct in the measurement too | A host wait per frame and twin | The same result for a wait |
| Clear the buffer when it is allocated | One call at init | `hipMemset` on device memory is asynchronous; `float_moment_hip` stays wrong with it | Does not work |
| Kernels write per-block partials, the host adds them | Nothing to clear | Three kernels and their readback rewritten; more to read back | A larger change for the same scores |
| Leave it, the `vmaf` tool is not affected | No change | A library user with two contexts gets wrong scores without an error | Wrong scores |

## Consequences

- **Positive**: the first frame of `float_moment_hip`, `vif_hip` and
  `adm_hip` in a later, larger context is the CPU's. All fourteen twins of
  the device test pass in three of three runs (eleven did before).
- **Negative**: none measured. The order of two calls has no cost; see the
  timings in the [HIP backend guide](../backends/hip/overview.md#a-frame-clears-its-accumulators-after-its-upload-adr-1427).
- **Neutral / follow-ups**: two earlier full-suite failures of
  `test_hip_upload_race` on `float_moment_hip`, which creates many contexts
  in one process, fit this defect but were not reproduced on demand. The
  motion twins are not in the device test: their first frame has no score.
  Guards: `test_hip_first_frame_clear_<twin>` (device, one binary per twin)
  and `test_hip_clear_after_upload_contract.py` (device-free, every HIP
  source, six planted regressions). Reopen when ROCm or the amdgpu driver on
  a reference workstation changes: revert one reorder and run the device test.

## References

- `req` (maintainer brief for the RC3 lanes, 2026-10-01): "Every fix gets a test that fails without it. Bugs you find on the way get fixed (own small PR when out of scope), not just recorded."
- [ADR-1423](1423-hip-adm-cpu-row-rounding.md) (where the defect was found,
  and `adm_hip`'s reorder), [ADR-1408](1408-hip-shared-frame-planes.md) (the
  shared plane upload), [ADR-0214](0214-gpu-parity-ci-gate.md).
- hipamd `ihipMemset()`:
  `projects/clr/hipamd/src/hip_memory.cpp` in `ROCm/rocm-systems` at tag
  `rocm-7.2.4`.
- `docs/state.md`: `T-HIP-FIRST-FRAME-ASYNC-CLEAR-OTHER-TWINS-2026-10-01`
  (closed by this decision),
  `T-HIP-ADM-FIRST-FRAME-STALE-ACCUMULATORS-2026-10-01`,
  `T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`.
