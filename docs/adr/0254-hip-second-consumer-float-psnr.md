<!-- markdownlint-disable MD013 -->
# ADR-0254: HIP second-consumer kernel — `float_psnr_hip` via mirrored kernel-template

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, hip, rocm, amd, kernel-template, fork-local

## Context

[ADR-0212](0212-hip-backend-scaffold.md) shipped the HIP backend as a
build-only scaffold (T7-10 audit half).
[ADR-0241](0241-hip-first-consumer-psnr.md) followed up with the
**first kernel-template consumer** (`integer_psnr_hip`), establishing
the per-frame async lifecycle the runtime PR (T7-10b) will eventually
fill in.

This ADR ships the **second consumer** — `float_psnr_hip` — to
exercise the same scaffold contract through a second feature with a
slightly different precision posture (`float` per-WG partials instead
of `uint64` SSE). The goal is to prove the kernel-template's shape
generalises across feature precisions before the runtime PR lands, so
that PR can swap helper bodies for live HIP calls without churning
either consumer's call-graph.

`float_psnr_cuda.c` is the natural twin to mirror: same algorithmic
shape as `integer_psnr_cuda.c`, slightly larger surface (bit-depth-aware
peak / clamp formula, float partials) at 272 LOC, single dispatch per
channel, single readback. The features are functionally
distinguishable so smoke-test coverage actually disambiguates lookup
behaviour rather than collapsing to the first-consumer assertion.

## Decision

### Land a second kernel-template consumer (`float_psnr_hip`), kernel body still deferred to T7-10b

The PR ships:

- **`core/src/feature/hip/float_psnr_hip.{c,h}`** — mirrors
  `core/src/feature/cuda/float_psnr_cuda.c`'s call graph
  verbatim: `init → context_new + lifecycle_init + readback_alloc +
  feature_name_dict`, `submit → submit_pre_launch + (kernel-launch +
  event-record + DtoH copy land in T7-10b)`, `collect → collect_wait
  score-emit (also T7-10b)`, `close → lifecycle_close +
  readback_free + dictionary_free + context_destroy`. The
  bit-depth-aware `peak` / `psnr_max` table is established at
  `init()` time so the eventual cross-backend numeric gate has
  nothing fork-specific to track. `init()` returns `-ENOSYS` because
  the template helpers do; the consumer's call-site shape is the
  load-bearing artefact this PR pins.
- **Registration**: `vmaf_fex_float_psnr_hip` is added to the
  `feature_extractor_list` in
  `core/src/feature/feature_extractor.c` under `#if HAVE_HIP`,
  immediately after the first consumer. Same posture as
  ADR-0241: registration succeeds, the
  `VMAF_FEATURE_EXTRACTOR_HIP` flag stays cleared until T7-10b, no
  device buffer-type plumbing is added yet.
- **Smoke test extension**: `core/test/test_hip_smoke.c` grows one
  sub-test (`test_float_psnr_hip_extractor_registered`) that mirrors
  the existing first-consumer registration assertion but for
  `float_psnr_hip`. The smoke test file's table-driven structure
  keeps `run_tests()` flat (15 sub-tests after this PR; the
  `readability-function-size` 15-branch budget is comfortably under
  the threshold).
- **Meson wiring**: `core/src/hip/meson.build` adds
  `../feature/hip/float_psnr_hip.c` to `hip_sources`. No new
  dependency — the consumer compiles on a stock Ubuntu runner
  without any AMD packages installed (same posture as ADR-0212 +
  ADR-0241).

### What stays on T7-10b

- Real `hipStreamCreate` / `hipMemcpyAsync` / `hipMallocAsync` bodies
  in `kernel_template.c`.
- Per-bpc kernel launch (`float_psnr_kernel_8bpc` /
  `float_psnr_kernel_16bpc` HIP twins).
- `VMAF_FEATURE_EXTRACTOR_HIP` flag flip on both consumers.
- Picture buffer-type plumbing for HIP-resident frames.
- Score emission (`vmaf_feature_collector_append_with_dict` with the
  log10 / clamp formula).

The runtime PR will keep this consumer's call-graph verbatim and
flip every `-ENOSYS` to a live error code, mirroring how the CUDA
twin handles failures today.

## Alternatives considered

### A. Pick `integer_motion_hip` as the second consumer

Rejected. `integer_motion_cuda.c` is 518 LOC with a stateful
single-frame-delay reference plane plus its own ring-buffer; mirroring
it before the runtime exists would force scaffold gymnastics around
state that has no observable effect (every frame's submit returns
`-ENOSYS`). `float_psnr_cuda.c` is half the LOC, single-dispatch,
no inter-frame state — the cleanest second consumer in the CUDA tree.

### B. Pick `integer_motion_v2_hip` (320 LOC)

Rejected on the same state-plumbing argument. v2 still carries the
single-frame-delay reference and the v1/v2 dual-feature emission
that doubles the score-collector surface area. Pre-runtime, that
extra surface is dead weight; post-runtime it's a one-PR follow-up
once the runtime path is proven on the simpler twin.

### C. Promote one of the existing scaffold stubs (`adm_hip` / `vif_hip` / `motion_hip`) to a kernel-template consumer

Rejected. The scaffold stubs in `adm_hip.c` / `vif_hip.c` /
`motion_hip.c` use a different ABI shape (`vmaf_hip_<feature>_init`
/ `_run` / `_destroy` C functions, not `VmafFeatureExtractor` struct
registration). They were scaffolded by ADR-0212 deliberately as
"backend-internal stubs", not consumers. Repurposing one would
collapse two distinct contracts (the kernel-internal C ABI versus
the feature-extractor registration ABI) into a single confused
shape. The fork keeps the stubs as scaffold-only placeholders the
runtime PR can wire up later, and grows kernel-template consumers
side-by-side as separate TUs.

### D. Defer the second consumer entirely until T7-10b lands

Rejected. The kernel-template's load-bearing claim is "every
fork-added HIP feature consumer goes through this contract". A
single consumer doesn't prove the contract generalises; a second
consumer at a different precision (float partials versus int64
SSE) does. Catching a contract-shape mismatch *before* the runtime
PR is dramatically cheaper than catching it after — once T7-10b
flips bodies to real HIP calls, every consumer-side regression
becomes runtime-visible and must be debugged against a real device.

## Consequences

- The HIP `feature_extractor_list[]` grows from 1 entry to 2.
- A caller asking for `vmaf --feature float_psnr_hip` now gets
  "extractor found, runtime not ready (-ENOSYS at init)" instead
  of "no such extractor".
- The smoke-test sub-test count grows from 14 to 15. Still under the
  `readability-function-size` 15-branch budget thanks to the
  table-driven `run_tests()` shape ADR-0241 introduced.
- T7-10b's surface is unchanged: same six kernel-template helper
  bodies to flip, same two consumers to validate against. The
  second consumer simply doubles the validation surface for free.
- No new build-time dependency. The scaffold-only build still
  succeeds on a stock Ubuntu runner without ROCm.
- Bit-exactness is not claimed; the kernels still don't exist.
  `/cross-backend-diff` integration lands with T7-10b.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP backend scaffold
  (T7-10 audit half).
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first kernel-template
  consumer (`integer_psnr_hip`); this ADR's predecessor.
- [ADR-0221](0221-gpu-kernel-template.md) — original CUDA kernel
  template that ADR-0241 mirrored onto HIP.
- [ADR-0246](0246-gpu-kernel-template.md) — kernel-template
  generalisation across GPU backends.
- `core/src/feature/cuda/float_psnr_cuda.c` — the CUDA reference
  whose call graph this consumer mirrors.
- `req` — user direction in T7-10b implementation prompt
  (paraphrased: "Land the next HIP runtime kernel body — pick the
  lightest CUDA twin").
