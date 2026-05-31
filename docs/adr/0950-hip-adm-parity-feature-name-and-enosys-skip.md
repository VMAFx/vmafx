<!-- markdownlint-disable MD013 MD060 -->
# ADR-0950: Fix symmetric "adm" vs "adm_hip" feature-name bug in test_hip_adm_parity and add ENOSYS skip

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: test, hip, parity, fork-local

## Context

The PR #451 audit on `core/test/test_hip_motion3_parity.c` (ADR-0949)
surfaced a sibling defect in the companion test
`core/test/test_hip_adm_parity.c`: both the CPU and the HIP branches of
`test_integer_adm_cpu_hip_parity()` invoked
`run_extractor("adm", ...)` — the upstream CPU extractor name — rather
than `run_extractor("adm_hip", ...)` for the HIP branch. The HIP
extractor is registered as `"adm_hip"` in
`core/src/feature/hip/integer_adm_hip.c` line 1391; the test silently
re-ran the CPU extractor twice and passed trivially. The
ADR-0539 parity claim (integer ADM CPU-vs-HIP, places=4) was therefore
not actually being exercised by the gate that was named after it.

Additionally, the test only handled the *no-HIP-device* skip path. On a
build with `enable_hip=true` but `enable_hipcc=false` (the common dev
posture on non-AMD hosts and on fork-CI runners that pin off the ROCm
toolchain), the `adm_hip` extractor's `init` / `submit` hits
`#ifndef HAVE_HIPCC` in `integer_adm_hip.c` lines 1014-1289 and returns
`-ENOSYS`. Before this fix the `mu_assert("vmaf_read_pictures failed",
!err)` would have fired loudly with no skip — the same posture
ADR-0949 documented for the motion3 sibling test.

## Decision

We will fix `core/test/test_hip_adm_parity.c` to:

1. Call `run_extractor("adm_hip", /*use_hip=*/1, ...)` for the HIP
   comparand (was `"adm"`), so the HIP kernels are actually exercised
   when the device + toolchain are both present.
2. Extract an `adm_submit_one_frame()` helper that detects `-ENOSYS`
   on the first-frame submit, tears down the partially-initialised
   `VmafContext` + `VmafHipState`, emits
   `[skip: HIP kernels not built (enable_hipcc=false)]`, and passes.

The CPU baseline call site (`run_extractor("adm", /*use_hip=*/0, ...)`)
and the `places=4` (1e-4) parity tolerance are unchanged. Real parity
drift still fails the test when the device + kernels are both present.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix feature name only; keep blanket `mu_assert` on `vmaf_read_pictures` | Smallest diff, scoped to the audit-named bug | Would still hard-fail on every `enable_hip=true, enable_hipcc=false` run — the test would *start* tripping after this PR rather than just silently passing | Pairs the bug fix with the same skip predicate ADR-0949 added to the motion3 sibling so the gate is meaningful across the fork's two-axis HIP build matrix |
| Delete the test and re-write from scratch against the metal motion3 template | Cleanest code, would also drop the CPU-baseline duplication shared by both sibling parity tests | Burns the ADR-0539 history; harder to review against the original | Deferred — the test's structure is sound, only the feature name was wrong and the skip predicate was incomplete |
| Gate on `HAVE_HIPCC` macro at compile time (`#ifdef`) and never compile the HIP branch when off | Eliminates the runtime branch entirely | Macro leaks into the test's TU when the runtime is present but kernels aren't; couples the test source to a build-time symbol that's already covered by `-ENOSYS` at runtime | The runtime `-ENOSYS` check mirrors how `vmaf_init` / `vmaf_use_feature` callers in user code would see it, which is what the gate is supposed to model |
| Replace `mu_assert` with a logging-only failure and continue regardless | Would surface the bug without blocking CI | Defeats the purpose of a parity gate; converts a hard contract into a noise channel | Rejected — a parity gate that doesn't fail is a placebo |

## Consequences

- **Positive**: the `integer_adm` CPU-vs-HIP parity claim cited in
  ADR-0539 is now actually enforced when the device + kernels are
  present; the gate stops passing trivially on every CI run.
- **Positive**: the test joins `test_hip_motion3_parity.c` in
  cleanly skipping on the `enable_hip=true, enable_hipcc=false` posture
  rather than hard-failing, removing the second of two known
  audit-flagged hard failures from `meson test --suite gpu`.
- **Negative**: hosts with HIP + HIPCC + a real device may now surface
  ADR-0214 parity drift that the buggy version was masking. This is
  the gate doing its job; any failure is a real bug and gets its own
  ADR.
- **Neutral / follow-ups**: the same audit pattern (symmetric
  `feature_name` argument on both CPU and GPU calls) should be
  spot-checked across `test_cuda_*_parity.c`, `test_sycl_*_parity.c`,
  `test_vulkan_*_parity.c`, and `test_metal_*_parity.c`. Tracked as
  T-PARITY-TEST-AUDIT-2026-05-31 in `docs/state.md`.

## References

- Sibling fix for motion3: ADR-0949
  (`docs/adr/_index_fragments/0949-hip-motion3-parity-enosys-skip.md`,
  not yet on `master` — lives on `fix/test-hip-motion3-parity` /
  PR #451).
- Original parity-claim ADR: ADR-0539 (integer ADM HIP kernel port).
- Cross-backend tolerance gate: ADR-0214 (places=4).
- HIP two-axis build matrix:
  `core/src/feature/hip/integer_adm_hip.c` lines 1014-1289,
  `core/src/meson.build` lines 128-166.
- Source: `req` — caller-supplied task brief explicitly flagged the
  bug shape: "calls `run_extractor('adm', ...)` for BOTH CPU and HIP
  branches, so it passes trivially via the CPU fallback instead of
  actually exercising the HIP kernel" (paraphrased to neutral
  English per CLAUDE.md user-quote-handling rule).
