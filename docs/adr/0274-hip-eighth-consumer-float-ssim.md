<!-- markdownlint-disable MD013 MD060 -->
# ADR-0274: HIP eighth kernel-template consumer — `float_ssim_hip`

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (T7-10b follow-up)
- **Tags**: `hip`, `gpu`, `feature-extractor`, `kernel-template`, `multi-dispatch`, `fork-local`

## Context

The first seven HIP kernel-template consumers
([ADR-0241](0241-hip-first-consumer-psnr.md),
[ADR-0254](0254-hip-second-consumer-float-psnr.md),
[ADR-0259](0259-hip-third-consumer-ciede.md),
[ADR-0260](0260-hip-fourth-consumer-float-moment.md),
[ADR-0266](0266-hip-fifth-consumer-float-ansnr.md),
[ADR-0267](0267-hip-sixth-consumer-motion-v2.md), and
[ADR-0273](0273-hip-seventh-consumer-float-motion.md)) are all
**single-dispatch**: each frame is processed in one kernel launch.
The runtime PR (T7-10b) needs at least one **multi-dispatch** consumer
in place before it lands so the helper-body flip can validate the
case where two kernels run on the picture stream with implicit
happens-before ordering between them.

This ADR adds the **eighth consumer** — `float_ssim_hip`, the HIP
twin of `core/src/feature/cuda/integer_ssim_cuda.c` (384 LOC,
which despite its filename registers `vmaf_fex_float_ssim_cuda` and
emits the `float_ssim` feature). It pins two new shapes:
**two-dispatch separable Gaussian** (horizontal pass writes five
intermediate float buffers, vertical pass reads them and writes
per-block float partials) and **`chars.n_dispatches_per_frame == 2`**
in the extractor's characteristics — every prior HIP consumer has
`n_dispatches_per_frame == 1`. The smoke test pins this value
explicitly so the runtime PR's dispatch-counter accounting can't
silently drift.

`float_ssim` carries a **v1 scale=1 constraint** matching `ssim_cuda`
and `ssim_vulkan`: auto-decimation rejects scale>1 with `-EINVAL` at
init time. Pinning this in the scaffold so a caller asking for
`float_ssim_hip:scale=2` sees a clean validation surface instead of
the kernel-template's `-ENOSYS`.

## Decision

We add `core/src/feature/hip/float_ssim_hip.{c,h}` as the eighth
kernel-template consumer. The TU mirrors the CUDA twin
call-graph-for-call-graph: identical state struct (modulo the
CUDA-driver `CUfunction` slots and the `VmafCudaBuffer *`-vs-`uintptr_t`
buffer slot type difference), identical `validate_dims` /
`init_dims` helpers (extracted to keep `init()` under the
`readability-function-size` budget), identical `init/submit/collect/close`
lifecycle, and identical `c1` / `c2` SSIM constants
(`L = 255.0`, `K1 = 0.01`, `K2 = 0.03`). `init()` returns `-ENOSYS`
once dimension validation passes, until T7-10b lands the runtime
helpers; the smoke test pins both the registration shape and
`chars.n_dispatches_per_frame == 2`; CHANGELOG and rebase-notes carry
the addition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `float_ssim_hip` (chosen) | Pins the multi-dispatch shape (`n_dispatches_per_frame == 2`); pins the five-intermediate-float-buffer pyramid that no prior consumer exercises; pins the v1 scale=1 `-EINVAL` validation surface so callers don't get a confusing `-ENOSYS` from a runtime-not-ready kernel for an input the kernel wouldn't have supported anyway. | 384 LOC is the largest unported CUDA twin in the smallest-twin tier (still under integer_ssim's 384 → integer_psnr_hvs's 492). | — |
| `float_motion_hip` (361 LOC) | Single-dispatch motion mirror. | Not multi-dispatch — leaves the runtime PR without a multi-dispatch consumer to validate against. | Picked alongside this one as the *seventh* consumer (ADR-0273) for the temporal+three-buffer-ping-pong shape. |
| `integer_psnr_hvs_cuda.c` (492 LOC) | Pins the per-DCT-block SSIM-like shape. | 28% larger than `integer_ssim_cuda.c`; complex DCT scratch buffers. | Defer to a later batch. |
| `integer_ms_ssim_cuda.c` (502 LOC) | Pins the multi-scale pyramid shape. | 30% larger; multi-scale pyramid is a more invasive shape than two-dispatch single-scale. | Defer to a later batch. |
| Skip the multi-dispatch consumer | Keeps the diff smaller. | Leaves the runtime PR without any consumer that exercises `n_dispatches_per_frame == 2` — every helper-body flip lands without that validation. | Adds a follow-up PR for no real saving. |

## Consequences

- **Positive**: The multi-dispatch shape is now pinned ahead of T7-10b.
  The runtime PR's helper-body flip can validate that two kernels on
  the picture stream with implicit happens-before ordering produce the
  expected per-block partials. The `chars.n_dispatches_per_frame == 2`
  invariant is asserted in the smoke test, so a refactor that
  accidentally drops that field to 1 fails the lookup contract.
- **Negative**: Adds two helper functions (`validate_dims_hip`,
  `init_dims_hip`) extracted from `init()` to keep the
  `readability-function-size` budget. The CUDA twin keeps everything
  inline; the HIP twin needs the extraction because it adds extra
  context-allocation steps the CUDA path doesn't. The AGENTS.md note
  pins this layout difference so a future refactor doesn't try to
  re-inline the helpers and bust the budget.
- **Neutral / follow-ups**: When the runtime PR lands a HIP
  device-buffer allocator, the five intermediate float buffer slots
  (`uintptr_t h_{ref_mu,cmp_mu,ref_sq,cmp_sq,refcmp}`) become its first
  five-buffer client. The CUDA twin's
  `VmafCudaBuffer *h_{ref_mu,cmp_mu,ref_sq,cmp_sq,refcmp}` field shape
  is the target the runtime PR will mirror.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP audit-first scaffold (T7-10).
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first consumer + kernel template.
- [ADR-0254](0254-hip-second-consumer-float-psnr.md) — second consumer.
- [ADR-0259](0259-hip-third-consumer-ciede.md) — third consumer.
- [ADR-0260](0260-hip-fourth-consumer-float-moment.md) — fourth consumer.
- [ADR-0266](0266-hip-fifth-consumer-float-ansnr.md) — fifth consumer (PR #340).
- [ADR-0267](0267-hip-sixth-consumer-motion-v2.md) — sixth consumer (PR #340).
- [ADR-0273](0273-hip-seventh-consumer-float-motion.md) — seventh consumer (this PR).
- [ADR-0246](0246-gpu-kernel-template.md) — GPU kernel-template pattern.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4` cross-backend gate.
- CUDA twin: `core/src/feature/cuda/integer_ssim_cuda.c` (registers `vmaf_fex_float_ssim_cuda`).
- Source: `req` (user dispatch) — "Add the seventh + eighth HIP runtime kernel-template consumers. ... Pick two more cleanest CUDA twins."
