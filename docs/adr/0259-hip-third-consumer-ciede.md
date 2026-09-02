# ADR-0259: HIP third-consumer kernel — `ciede_hip` via mirrored kernel-template

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, hip, rocm, amd, kernel-template, fork-local

## Context

[ADR-0212](0212-hip-backend-scaffold.md) shipped the HIP backend as a
build-only scaffold. [ADR-0241](0241-hip-first-consumer-psnr.md) added
the first kernel-template consumer (`integer_psnr_hip`). PR #324 /
[ADR-0254](0254-hip-second-consumer-float-psnr.md) is in flight as the
second consumer (`float_psnr_hip`). The runtime PR (T7-10b) is still
pending; until it lands, every kernel-template helper body returns
`-ENOSYS` so consumer `init()` calls surface that error verbatim.

This ADR ships the **third consumer** — `ciede_hip` — to widen the
kernel-template's pre-runtime validation surface with a feature whose
submit path **intentionally bypasses** the template's
`submit_pre_launch` helper (the ciede CUDA twin inlines the wait
because its kernel writes one float per block — no atomic, no memset
required). Pinning that bypass shape pre-runtime keeps the
runtime-PR's diff small: T7-10b flips helper bodies, but does not
have to invent a new "no-memset" template variant.

`integer_ciede_cuda.c` (243 LOC) is the cleanest CUDA twin to mirror
after the two already-claimed PSNR twins: single dispatch per frame,
single readback (per-block float partials), no inter-frame state, no
fork-specific tweaks. Its precision posture (per-block float partials
plus host double accumulation, per ADR-0187) sits between
`integer_psnr_hip`'s int64 SSE and `float_psnr_hip`'s float partials.

## Decision

### Land `ciede_hip` as the third kernel-template consumer; runtime body still deferred

The PR ships:

- **`core/src/feature/hip/ciede_hip.{c,h}`** — mirrors
  `core/src/feature/cuda/integer_ciede_cuda.c`'s call graph
  verbatim: `init → context_new + lifecycle_init + readback_alloc +
  feature_name_dict`, `submit → -ENOSYS` (the runtime PR will fill
  in the live `hipStreamWaitEvent` + dispatch + event-record + DtoH
  copy chain — note the **intentional bypass** of
  `submit_pre_launch`, mirroring the CUDA twin's no-memset path),
  `collect → collect_wait + score-emit (T7-10b)`, `close →
  lifecycle_close + readback_free + dictionary_free +
  context_destroy`. The 16×16 workgroup tile constants
  (`CIEDE_HIP_BX` / `CIEDE_HIP_BY`) are kept verbatim from the CUDA
  twin so the runtime PR's `partials_count` math agrees.
- **Registration**: `vmaf_fex_ciede_hip` is added to
  `feature_extractor_list` in
  `core/src/feature/feature_extractor.c` under `#if HAVE_HIP`,
  immediately after the second consumer. Same posture as ADR-0241 /
  ADR-0254: registration succeeds, `VMAF_FEATURE_EXTRACTOR_HIP`
  flag stays cleared.
- **Smoke test extension**: `core/test/test_hip_smoke.c` grows
  one sub-test (`test_ciede_hip_extractor_registered`).
- **Meson wiring**: `core/src/hip/meson.build` adds
  `../feature/hip/ciede_hip.c` to `hip_sources`. No new dependency
  — the consumer compiles on a stock Ubuntu runner without any AMD
  packages installed.

### What stays on T7-10b

- Real `hipStreamCreate` / `hipMemcpyAsync` / `hipMallocAsync` bodies
  in `kernel_template.c`.
- Per-bpc kernel launch (`ciede_kernel_8bpc` / `ciede_kernel_16bpc`
  HIP twins).
- `VMAF_FEATURE_EXTRACTOR_HIP` flag flip on every consumer.
- Picture buffer-type plumbing for HIP-resident frames.
- Score emission with the `45 - 20*log10(mean_dE)` formula.

The runtime PR will keep this consumer's call-graph verbatim and flip
every `-ENOSYS` to a live error code, mirroring how the CUDA twin
handles failures today.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `ciede_hip` (chosen) | 243 LOC, single dispatch, intentional `submit_pre_launch` bypass widens validation surface, CUDA twin stable | Pre-runtime, the bypass shape is a paper-only contract | The bypass-shape coverage is exactly the value-add of a third consumer at this stage. |
| `integer_motion_hip` | Cleaner CUDA twin in raw LOC | 503 LOC + stateful single-frame-delay reference plane + ring-buffer; mirroring before runtime forces scaffold gymnastics around state with no observable effect (every submit returns `-ENOSYS`) | Higher complexity / LOC for less validation surface. |
| `float_ansnr_hip` | 298 LOC, similar precision posture to `float_psnr` | Substantively duplicates the second consumer's precision posture (float partials) — adds little new validation | Smaller delta vs. ADR-0254. |
| Defer until T7-10b | Avoids one round of paper-only scaffold | A single (or two) consumer doesn't prove the contract generalises; catching contract drift post-runtime is dramatically more expensive — every regression then needs a real device to debug | The fork's pattern is "validate scaffolds before runtimes" (Vulkan T5-1 → T5-1b cadence). |

## Consequences

- **Positive**: kernel-template's "no-memset bypass" path is now
  pinned by a smoke-tested consumer. The runtime PR can flip helper
  bodies without inventing a new template variant for ciede.
- **Positive**: HIP `feature_extractor_list[]` grows by one row
  (third row); a caller asking for `vmaf --feature ciede_hip` now
  gets "extractor found, runtime not ready (-ENOSYS at init)"
  instead of "no such extractor".
- **Neutral**: smoke-test sub-test count grows by one (was 15 after
  ADR-0254, becomes 16 with this PR plus ADR-0260). Still fits the
  table-driven `run_tests()` shape ADR-0241 introduced (the
  `readability-function-size` 15-branch budget applies to the
  function's branches, not the table length).
- **Neutral**: T7-10b's surface is unchanged — same six
  kernel-template helper bodies to flip, plus per-feature kernel
  launches.
- **Neutral**: bit-exactness is not claimed; the kernels still don't
  exist. `/cross-backend-diff` integration lands with T7-10b.
- **Negative**: the PR introduces another file pair pinned at
  `-ENOSYS` until T7-10b. If T7-10b slips, the `-ENOSYS` rows accrue
  small maintenance cost (re-rebase, re-format on touched files).

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP scaffold-only PR.
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first kernel-template
  consumer (`integer_psnr_hip`).
- [ADR-0254](0254-hip-second-consumer-float-psnr.md) — second
  kernel-template consumer (`float_psnr_hip`); in flight as PR #324.
- [ADR-0221](0221-gpu-kernel-template.md) — original CUDA kernel
  template that ADR-0241 mirrored onto HIP.
- [ADR-0187](0187-ciede-vulkan.md) — ciede precision /
  `places=4` empirical floor argument; carries to the HIP twin via
  the per-block float partials + host double accumulation pattern.
- `core/src/feature/cuda/integer_ciede_cuda.c` — the CUDA
  reference whose call graph this consumer mirrors.
- `req` — user direction in T7-10b implementation prompt
  (paraphrased: "Land the third and fourth HIP runtime
  kernel-template consumers; pick the cleanest CUDA twins").
