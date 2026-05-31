<!-- markdownlint-disable MD013 MD060 -->
# ADR-0241: HIP first-consumer kernel — `integer_psnr_hip` via mirrored kernel-template

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, hip, rocm, amd, kernel-template, fork-local

## Context

[ADR-0212](0212-hip-backend-scaffold.md) shipped the HIP backend as a
build-only scaffold (T7-10 audit half): public header, backend tree
under `core/src/hip/`, three feature stubs (ADM / VIF / motion), a
CI matrix lane (`Build — Ubuntu HIP (T7-10 scaffold)`), and a 9-sub-test
smoke pinning the `-ENOSYS` contract for every public C-API entry
point. The runtime (T7-10b) and the first real kernel were
deliberately deferred to a follow-up.

This ADR is that follow-up's first half: land the **first kernel-template
consumer** so the per-frame async lifecycle the runtime PR will need
to implement is anchored to a concrete consumer, mirroring the role
[ADR-0221](0221-gpu-kernel-template.md) and
`core/src/feature/cuda/integer_psnr_cuda.c` play on the CUDA side.

The CUDA kernel template (`core/src/cuda/kernel_template.h`) is the
canonical scaffolding for fork-added GPU feature kernels — every CUDA
feature extractor migrated to it under T-GPU-DEDUP-4..17. HIP's
runtime API is a near-clone of CUDA's (`hipStream_t` ↔ `CUstream`,
`hipEvent_t` ↔ `CUevent`, `hipMallocAsync` ↔ `cuMemAllocAsync`, etc.),
so the template's shape ports one-to-one. Establishing the mirror
*now*, against the absent runtime, gives the runtime PR a clear
contract to fill in.

## Decision

### Land the kernel-template mirror + one consumer (PSNR), kernel-body deferred to T7-10b

The PR ships:

- **`core/src/hip/kernel_template.h`** — declares
  `VmafHipKernelLifecycle` (private stream + submit/finished event
  pair) and `VmafHipKernelReadback` (device accumulator + pinned host
  slot), plus six lifecycle helpers
  (`vmaf_hip_kernel_lifecycle_init/_close`,
  `vmaf_hip_kernel_readback_alloc/_free`,
  `vmaf_hip_kernel_submit_pre_launch`,
  `vmaf_hip_kernel_collect_wait`). Field-for-field mirror of the CUDA
  template; HIP runtime types cross the ABI as `uintptr_t` to keep the
  header free of `<hip/hip_runtime.h>` (same convention
  [`libvmaf_hip.h`](../../core/include/libvmaf/libvmaf_hip.h) uses
  per ADR-0212).
- **`core/src/hip/kernel_template.c`** — out-of-line bodies for
  every helper. Returns `-ENOSYS` until the runtime PR (T7-10b) swaps
  in real `hipStreamCreate` / `hipMemsetAsync` / `hipStreamSynchronize`
  calls. Close + free helpers are no-ops on zero handles (matches the
  CUDA "safe to call on a partially-initialised lifecycle" contract).
- **`core/src/feature/hip/integer_psnr_hip.{c,h}`** — first consumer.
  Mirrors `core/src/feature/cuda/integer_psnr_cuda.c`'s call graph
  verbatim: `init → context_new + lifecycle_init + readback_alloc +
  feature_name_dict`; `submit → submit_pre_launch + (kernel-launch +
  event-record + DtoH copy land in T7-10b)`; `collect → collect_wait
  score-emit (also T7-10b)`; `close → lifecycle_close +
  readback_free + dictionary_free + context_destroy`. Init returns
  `-ENOSYS` (because the template helpers do); the consumer's
  call-site shape is the load-bearing artefact this PR pins.
- **Registration**: `vmaf_fex_psnr_hip` is added to the
  `feature_extractor_list` in
  `core/src/feature/feature_extractor.c` under `#if HAVE_HIP`. A
  caller asking for `psnr_hip` by name now gets "extractor found,
  runtime not ready (-ENOSYS at init)" instead of "no such
  extractor". A new `VMAF_FEATURE_EXTRACTOR_HIP = 1 << 6` flag is
  reserved in the enum so the runtime PR can adopt it without an
  ABI shuffle; the first consumer does not set the flag yet because
  the picture buffer-type plumbing (HIP-device buffer type tag) is
  the runtime PR's responsibility.
- **Smoke test extension**: `core/test/test_hip_smoke.c` grows
  five sub-tests pinning the kernel-template scaffold contract
  (`lifecycle_init` / `readback_alloc` return `-ENOSYS`;
  `lifecycle_close` / `readback_free` are no-ops on zero handles;
  `vmaf_get_feature_extractor_by_name("psnr_hip")` returns the
  registered extractor). 14/14 pass on `-Denable_hip=true`.

### Out-of-line helpers, not `static inline`

The CUDA template uses `static inline` because every helper unwraps
straight onto the `CudaFunctions` driver-table (`cu_state->f->...`)
that exists today. HIP has no equivalent driver loader yet — the
runtime PR is still pending. The HIP template helpers therefore live
in a paired `kernel_template.c` so the runtime PR can replace bodies
without recompiling every consumer TU. When the runtime lands, the
header and consumer call-sites stay verbatim; the .c bodies flip
from `-ENOSYS` to live HIP calls.

### No `.cu` / `.hip` device blob in this PR

The CUDA reference also ships `psnr_score.cu` (compiled to PTX, loaded
via `cuModuleLoadData`). The HIP equivalent (a `psnr_score.hip`
fatbin) requires `hipcc` + ROCm 6.x at build time, which the scaffold
explicitly has no requirement for (per ADR-0212). The kernel-source
artefact arrives with the runtime PR alongside the live driver-table
and pinned-host allocator. The `integer_psnr_hip.h` placeholder
documents the symbol the runtime PR will declare.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **PSNR as first consumer** *(chosen)* | Simplest reduction-only feature on the CUDA side (one int64 SSE accumulator, one dispatch per channel, no IIR / pyramid state); already the documented reference consumer of `cuda/kernel_template.h` per ADR-0221; one device-side accumulator + one pinned host readback maps cleanly onto the kernel template's struct shape; `psnr_y` is a single emitted feature so the consumer stays under 250 LOC; the CUDA twin's bit-exactness vs CPU was straightforward (int64 accumulation matches `sse_line_8/16` byte-for-byte) so the runtime PR has a known-good reference to compare against | Luma-only in v1 (chroma deferred); not the most exciting kernel | Best signal-to-noise for a first port — the consumer establishes the template-mirror contract without the noise of a multi-band reduction or a hand-rolled IIR |
| Motion as first consumer | Also reduction-only, similar lifecycle | Already touches frame-sync (`VMAF_FEATURE_EXTRACTOR_TEMPORAL`), holds previous-reference state, has two register-level reductions (SAD + filter) → larger blast radius for a "shape the template port" PR | Reject — too much per-feature boilerplate to read past for the first port |
| ADM as first consumer | Most representative of the CUDA backend's complexity (multi-scale, DWT, CSF) | 4 dispatches per frame, 6 device-side accumulators, multi-band readback — the first consumer should pin the *minimum* template surface, not the maximum | Reject — ADM is the right *last* port in the runtime PR sweep, not the first |
| VIF as first consumer | The `vif_hip.c` stub already exists, so registration is "free" | VIF needs scratch buffers (filter pyramid), warp-level reductions across multiple bands, and an IIR-style state — same complexity argument as ADM | Reject — VIF is the kernel that the existing stub TU should host once the runtime arrives, but it's not the right *first* consumer |
| Inline helpers (mirror CUDA's `static inline`) | One fewer TU to compile; matches the CUDA template line-for-line | Inline bodies that return `-ENOSYS` would compile into every consumer TU; flipping to a real HIP call later would force every consumer to recompile | Reject — out-of-line gives the runtime PR a single editing target |
| Skip kernel-template mirror; port PSNR directly into `integer_psnr_hip.c` open-coded (no helpers) | One fewer header to maintain; matches what motion / VIF stubs do today | Establishes a precedent that every HIP kernel hand-rolls its own stream + event lifecycle — exactly the duplication T-GPU-DEDUP-4..17 spent 13 PRs deduplicating on the CUDA side | Reject — the template is the load-bearing artefact this PR ships; the PSNR consumer is the proof it ports cleanly |
| Land both the runtime + first consumer in one PR | One round of review, real numerics from day one | Runtime PR needs ROCm SDK in CI, which means the matrix lane has to install and link `amdhip64`; the audit-first split (per ADR-0212) explicitly defers that to T7-10b so this PR can stay ROCm-SDK-free | Reject — the audit-first cadence ADR-0212 / ADR-0175 / ADR-0173 share is well-validated; cutting it short here regresses the review story for no gain |
| Use `hipify-perl` to auto-translate `psnr_score.cu` for this PR | Free first kernel; matches the prompt hint that "HIPify will translate ~95 % of CUDA template code automatically" | The PR explicitly does not ship a kernel-source blob (no `hipcc` in CI), so `hipify`'s output has nothing to consume; would land a translated artefact that doesn't get compiled | Reject — `hipify` is an excellent porting *tool* for the runtime PR; this PR's scope ends at the consumer host scaffolding |

## Consequences

**Positive:**

- The runtime PR (T7-10b) has a concrete consumer's call-graph to fill
  in — no template ambiguity. Bodies flip from `-ENOSYS` to live HIP
  one helper at a time; consumer call-sites stay verbatim.
- The `psnr_hip` extractor name is registered in the feature engine,
  so callers asking by name from CLI / MCP / Python see "found,
  runtime not ready" not "unknown feature". This is a strictly better
  failure surface than the previous registry-omits-it state and is
  pinned by the smoke test.
- The kernel-template mirror caps duplication on the HIP side at the
  same point T-GPU-DEDUP-4..17 capped it on CUDA — every future HIP
  kernel calls the same five helpers instead of hand-rolling stream +
  event boilerplate.
- Build matrix's `Build — Ubuntu HIP (T7-10 scaffold)` lane now also
  exercises the kernel-template mirror + the consumer's call graph,
  catching template-ABI bit-rot the moment any future PR drifts the
  helper signatures.

**Negative:**

- Adds two new TUs (`kernel_template.c`, `integer_psnr_hip.c`) that
  are stub-shaped today. The runtime PR has to keep both updated when
  it swaps the bodies; the AGENTS.md invariant note pins this contract.
- The HIP feature-flag bit (`VMAF_FEATURE_EXTRACTOR_HIP = 1 << 6`)
  is reserved-but-unused in this PR; consumers reading the enum may
  briefly wonder why no extractor sets it. Mitigated by the inline
  comment on the enum declaration that points at T7-10b.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP scaffold-only
  audit-first PR (T7-10).
- [ADR-0221](0221-gpu-kernel-template.md) — CUDA kernel-template
  decision; this ADR is the HIP mirror.
- [ADR-0175](0175-vulkan-backend-scaffold.md) — Vulkan
  scaffold-then-runtime split, the cadence template ADR-0212 / this
  ADR / T7-10b reproduce.
- [`docs/backends/hip/overview.md`](../backends/hip/overview.md) —
  user-facing HIP backend status doc; updated in this PR to reflect
  the first-consumer registration flip.
- [`docs/backends/kernel-scaffolding.md`](../backends/kernel-scaffolding.md) —
  template migration guide; updated with the HIP mirror entry.
- [Backlog](../../BACKLOG.md) — T7-10 (closes audit half + first
  consumer; runtime + remaining kernels remain in T7-10b).
- `req`: per user direction, "ship the first real HIP feature kernel"
  → resolved as "ship the kernel-template mirror + the consumer's
  host scaffolding; kernel body lands in T7-10b once the HIP toolchain
  is available in CI" (no ROCm SDK on dev box; runtime PR cadence
  matches ADR-0212's audit-first split).
