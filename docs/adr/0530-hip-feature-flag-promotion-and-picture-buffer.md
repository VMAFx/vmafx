<!-- markdownlint-disable MD060 -->
# ADR-0530: HIP feature-extractor flag promotion and HIP_DEVICE picture-buffer type

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `dispatch`, `feature-extractor`

## Context

ADR-0519 implemented `vmaf_hip_import_state()` so the HIP backend
loads end-to-end on AMD ROCm hosts. To minimise blast-radius the
import-state PR deliberately left every HIP feature extractor with
the `VMAF_FEATURE_EXTRACTOR_HIP` flag bit cleared and skipped the
HIP-aware dispatch wiring in `compute_fex_flags()`. The
documented consequence was that `--backend hip` runs returned
bit-exact CPU scores: the dispatch routed every feature through
its CPU twin because (a) `compute_fex_flags()` returned no HIP
bit, and (b) `vmaf_get_feature_extractor_by_feature_name()` skipped
unflagged extractors when a flag mask was non-zero.

Several follow-up tasks all share the same blocker: the HIP kernels
that have been ported to real `hipModuleLaunchKernel` consumers
(starting with `integer_motion_hip`, ADR-0468 scaffold) cannot be
exercised from the CLI because the dispatch will not pick them. The
profiling, perf, and cross-backend-parity workstreams all need a
HIP-flagged extractor that actually runs the HIP path.

## Decision

Promote `VMAF_FEATURE_EXTRACTOR_HIP` on `vmaf_fex_integer_motion_hip`
(the first verified HIP kernel) and wire the dispatch so HIP-flagged
extractors are selectable when a HIP state has been imported. The
wiring is four changes:

1. Add `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE` to the picture-buffer-type
   enum (reserved for the future HIP picture pool; current HIP TUs
   accept HOST buffers and perform their own HtoD copy).
2. `feature/feature_extractor.c::vmaf_feature_extractor_context_extract()`
   rejects HIP-flagged extractors that receive a CUDA / SYCL / Vulkan
   device buffer, and rejects non-HIP extractors that receive a
   `HIP_DEVICE` buffer (symmetric to the existing CUDA check).
3. `libvmaf.c::compute_fex_flags()` adds `VMAF_FEATURE_EXTRACTOR_HIP`
   when `vmaf->hip.state` is set (host-pic only, like Vulkan — no
   gpumask gate).
4. `feature/feature_extractor.c::vmaf_get_feature_extractor_by_feature_name()`
   adds a fallback pass: after the preferred-flag match misses, retry
   without the flag filter so a HIP build with incomplete kernel
   coverage falls back to CPU twins for unflagged features. Without
   this fallback, turning on the HIP flag mask would break the
   default VMAF model (only motion / vif / ssimu2 have HIP-flagged
   registrations today; adm2 / motion2 / etc. would fail to resolve).

The PR also drains HIP-flagged extractors' `gpu_pending` final-frame
collect in `flush_context_serial()` (mirroring the SYCL pattern)
and routes the HIP extractor's collect-time writes through
`vmaf_feature_collector_append_with_dict()` so the encoded
option-aware key matches what `vmaf_predict_score_at_index()` looks
up. Both are pre-existing ADR-0519 latent bugs that surface only
once the flag bit flips on. The same change also wires the
previously-unregistered `vmaf_fex_integer_motion_hip` extractor
into the meson build and the registry list.

`vmaf_fex_integer_vif_hip` is intentionally **un-flagged** in the
same PR: it had `VMAF_FEATURE_EXTRACTOR_HIP` set speculatively in
its ADR-0254 / batch-1 commit but trips a GPU memory access fault
on the first frame when the flag mask actually picks it. Promoting
it requires its own kernel-level fix and its own ADR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Promote ALL HIP-flagged extractors at once | One PR closes the whole gap | `integer_vif_hip` crashes on first frame; would regress every `--backend hip` user | Recommended: gate per-extractor on a verified end-to-end run |
| Add a per-feature `requires_hip_state` gate instead of the flag bit | No global behavioural change | Duplicates the per-feature flag-bit pattern that CUDA / SYCL / Vulkan / Metal already use | Pattern symmetry beats the marginal isolation win |
| Skip the buffer-type plumbing until the HIP picture pool lands | Smaller diff | Future HIP-pool work then has to revisit the dispatch site twice; cross-GPU mismatch checks stay one-sided | Reserving the tag now costs one enum entry and lets the dispatch reject mixed backings in one place |
| Reject the fallback pass; require complete HIP coverage before enabling the flag | "Clean" semantics — selected backend must own every feature | Would require porting 8+ extractors before a single end-to-end HIP run works | The CUDA backend's complete coverage is the exception, not the contract; matching SYCL / Vulkan's partial-coverage posture is the right baseline |

## Consequences

- **Positive**:
  - `--backend hip --feature integer_motion` actually dispatches the
    HIP kernel (`AMD_LOG_LEVEL=3` shows 48 HSACO launches of
    `calculate_motion_score_kernel_8bpc` on a 48-frame clip).
  - The dispatch test harness (test_hip_smoke) now pins the
    flag-promotion contract: registry lookup + flag-aware
    `_by_feature_name` resolution.
  - Future HIP kernel ports follow a documented promotion pattern
    (set the flag bit, verify the reproducer, add a smoke assertion).
- **Negative**:
  - The fallback pass in `_by_feature_name` is now a documented
    contract — future GPU backend work cannot assume "flag set ⇒
    full coverage". Pre-existing CUDA behaviour is preserved
    because CUDA has full coverage today.
  - Cross-backend numerical parity for HIP `integer_motion` is
    "places=4" w.r.t. CPU (HIP vmaf=76.712514, CPU vmaf=76.66783 on
    the Netflix `src01_hrc00/hrc01` pair). Within the ADR-0214
    `places=4` gate; the residual is the known motion3-boot offset
    documented by the CUDA twin's ADR.
- **Neutral / follow-ups**:
  - Open issue: `integer_vif_hip` and `ssimulacra2_hip` remain
    unflagged pending kernel-level GPU memory access fault
    diagnosis. Each gets its own PR + ADR when fixed.
  - `motion_score` HSACO blob added to the meson `hip_kernel_sources`
    map; HIP HSACO total now 12 kernels.

## References

- [ADR-0523](0523-hip-integer-motion-extractor-registration.md) —
  immediate parent (PR #1283). Added the `extern` declaration,
  registry entry, and meson.build source-list entry for
  `vmaf_fex_integer_motion_hip`. This PR (ADR-0530) takes the
  next step: flags the extractor for HIP dispatch and adds the
  runtime wiring needed for the model-driven path to pick it.
- [ADR-0519](0519-hip-import-state-implementation.md) — HIP
  `vmaf_hip_import_state()` implementation; the grandparent.
- [ADR-0468](0468-hip-integer-motion-port.md) — `integer_motion_hip`
  scaffold + kernel port (the kernel this PR makes selectable).
- [ADR-0212](0212-hip-backend-scaffold.md) — HIP scaffold + audit-first
  posture (the original `-ENOSYS` contract).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — places=4 cross-backend
  numerical gate (the contract HIP joins).
- [ADR-0241](0241-hip-first-consumer-psnr.md) — kernel-template mirror
  the original "flag bit reserved but cleared" pattern.
- Source: `req` (user direct request to "promote the
  `VMAF_FEATURE_EXTRACTOR_HIP` flag bit so HIP-flagged extractors
  actually dispatch to HIP kernels instead of falling back to CPU
  twins").
