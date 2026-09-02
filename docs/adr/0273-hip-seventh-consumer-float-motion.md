<!-- markdownlint-disable MD013 MD060 -->
# ADR-0273: HIP seventh kernel-template consumer — `float_motion_hip`

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (T7-10b follow-up)
- **Tags**: `hip`, `gpu`, `feature-extractor`, `kernel-template`, `temporal`, `fork-local`

## Context

The HIP backend's kernel-template scaffold ([ADR-0241](0241-hip-first-consumer-psnr.md))
needs a stable quorum of consumers before the runtime PR (T7-10b) can
flip the `kernel_template.c` helper bodies from `-ENOSYS` to live HIP
calls. Each consumer pins a *shape* of usage that the runtime PR's
helper bodies must satisfy. Six are already accepted:
1-counter atomic readback (`integer_psnr_hip`, ADR-0241);
float-partial readback (`float_psnr_hip`, ADR-0254);
single-float-per-block bypass (`ciede_hip`, ADR-0259);
four-counter atomic readback (`float_moment_hip`, ADR-0260);
interleaved (sig, noise) per-block float partials (`float_ansnr_hip`,
ADR-0266); and the temporal-extractor `flush()` + ping-pong shape
(`motion_v2_hip`, ADR-0267, in PR #340).

This ADR adds a **seventh consumer** — `float_motion_hip`, the HIP
twin of `core/src/feature/cuda/float_motion_cuda.c` (361 LOC, the
smallest unported CUDA twin after the six already shipped or in
flight). It pins a new shape: **per-WG SAD float partials computed
against a blurred-frame ping-pong, with a separate raw-pixel cache
buffer**. Same `submit_pre_launch` bypass as `ciede_hip` (no atomic,
no memset required), but introduces a *three-buffer* ping-pong
(`uintptr_t ref_in` + `uintptr_t blur[2]`) that no prior consumer
exercises — `motion_v2_hip` only carries a two-slot raw-pixel
ping-pong.

## Decision

We add `core/src/feature/hip/float_motion_hip.{c,h}` as the seventh
kernel-template consumer. The TU mirrors the CUDA twin
call-graph-for-call-graph: identical state struct (modulo the
CUDA-driver `CUfunction` slots and the `VmafCudaBuffer *`-vs-`uintptr_t`
buffer slot type difference), identical `init/submit/collect/close`
lifecycle, identical `flush()` tail-frame motion2 emission shape, and
the same `motion_force_zero` short-circuit posture (`fex->extract`
swap with the kernel-template helpers skipped). `init()` returns
`-ENOSYS` until T7-10b lands the runtime helpers; the smoke test pins
the registration shape plus the `VMAF_FEATURE_EXTRACTOR_TEMPORAL`
flag bit; CHANGELOG and rebase-notes carry the addition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `float_motion_hip` (chosen) | Smallest unported CUDA twin (361 LOC) after the six already in master/flight; pins the **three-buffer ping-pong** shape (raw-pixel cache + blurred-frame ping-pong) that no prior consumer exercises; the `motion_force_zero` short-circuit posture (fex->extract swap) is also new to the HIP tree. | Introduces three `uintptr_t` buffer slots on top of the kernel-template's readback bundle. The runtime PR (T7-10b) will need a HIP buffer-alloc helper before it can populate them — the same blocker `motion_v2_hip` already creates. | — |
| `integer_ssim_hip` (384 LOC) | Two-dispatch + five intermediate float buffers shape. | Slightly larger; chosen as the *eighth* consumer in this same PR ([ADR-0274](0274-hip-eighth-consumer-float-ssim.md)) because the two shapes are complementary. | Picked alongside this one. |
| `integer_motion_hip` (518 LOC) | Pins the multi-dispatch motion shape. | 43% larger than `float_motion_cuda.c`; carries a four-buffer pyramid (blurred + decimated) that's a worse fit for "smallest unported CUDA twin". | Defer to a later batch. |
| Skip the seventh consumer | Keeps the diff smaller. | Leaves the runtime PR (T7-10b) without a third-temporal-extractor consumer to validate `flush()` + the three-buffer ping-pong against. | Adds a follow-up PR for no real saving. |

## Consequences

- **Positive**: One more concrete shape (raw-pixel cache + blurred-frame
  ping-pong + per-WG SAD float partials) is now pinned ahead of T7-10b,
  reducing the surface area the runtime PR has to validate against. The
  runtime PR's cross-backend-diff gate
  ([ADR-0214](0214-gpu-parity-ci-gate.md)) inherits a seventh consumer
  with no kernel work: just the standard helper-body flip plus the
  HIP buffer-alloc helper that `motion_v2_hip` already needs.
- **Negative**: One more `#if HAVE_HIP` extractor row in
  `feature_extractor.c` to keep aligned with the runtime PR's
  picture buffer-type plumbing. The `motion_force_zero` short-circuit
  invariant (the `fex->extract` swap) needs to stay aligned with the
  CUDA twin on every refactor.
- **Neutral / follow-ups**: AGENTS.md note pins the three-buffer
  ping-pong shape as a load-bearing invariant. The runtime PR will
  swap the `-ENOSYS` body for live calls and the three `uintptr_t`
  slots for real device-buffer handles, without touching this TU's
  `init/submit/collect/close` call sites.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP audit-first scaffold (T7-10).
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first consumer + kernel template.
- [ADR-0254](0254-hip-second-consumer-float-psnr.md) — second consumer (`float_psnr_hip`, PR #324).
- [ADR-0259](0259-hip-third-consumer-ciede.md) — third consumer (`ciede_hip`, PR #330).
- [ADR-0260](0260-hip-fourth-consumer-float-moment.md) — fourth consumer (`float_moment_hip`, PR #330).
- [ADR-0266](0266-hip-fifth-consumer-float-ansnr.md) — fifth consumer (`float_ansnr_hip`, PR #340).
- [ADR-0267](0267-hip-sixth-consumer-motion-v2.md) — sixth consumer (`motion_v2_hip`, PR #340).
- [ADR-0274](0274-hip-eighth-consumer-float-ssim.md) — eighth consumer (`float_ssim_hip`, this PR).
- [ADR-0246](0246-gpu-kernel-template.md) — GPU kernel-template pattern.
- CUDA twin: `core/src/feature/cuda/float_motion_cuda.c`.
- Source: `req` (user dispatch) — "Add the seventh + eighth HIP runtime kernel-template consumers. ... Pick two more cleanest CUDA twins."
