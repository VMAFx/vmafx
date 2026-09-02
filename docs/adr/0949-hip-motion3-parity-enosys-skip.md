<!-- markdownlint-disable MD060 -->
# ADR-0949: HIP motion3 parity test skips cleanly when HIPCC kernels are not built

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: hip, tests, bugfix, fork-local, dx

## Context

The fork's `meson test --suite gpu` invocation runs
`core/test/test_hip_motion3_parity.c`, a cross-backend assertion that
compares the CPU and HIP variants of `VMAF_integer_feature_motion3_score`
to within `places=4` (1e-4, per ADR-0214).  The test was authored under
the assumption that `vmaf_hip_state_init()` failure was a sufficient
skip predicate for "HIP is not usable on this host."

That assumption is incorrect for the fork's two-axis HIP build matrix.
`enable_hip=true` flips the *runtime* (HSA agent probing,
`hipGetDeviceCount`, `vmaf_hip_state_init`); `enable_hipcc=true`
separately flips the *device-kernel embed pipeline* that compiles the
`*.hip` sources into HSACO blobs (`core/src/meson.build` lines 128-166).
The two flags are independent: a developer or CI runner can have a
ROCm runtime + visible GPU yet build without `enable_hipcc=true` to
avoid the toolchain cost.

In that posture, `vmaf_hip_state_init()` succeeds, the test proceeds to
`vmaf_use_feature("motion_hip")` (registration only — also succeeds),
and the first `vmaf_read_pictures()` call invokes `init_fex_hip` and
`submit_fex_hip`, both of which hit `#ifndef HAVE_HIPCC` and return
`-ENOSYS` (`core/src/feature/hip/integer_motion_hip.c` lines 442-553).
The test's `mu_assert("HIP: vmaf_read_pictures failed", !err)` then
fails the whole test with no diagnostic distinguishing
"kernels not compiled" from "real bug."

PR #443's audit flagged this as a pre-existing failure that surfaced
on `meson test --suite gpu` against an `enable_hipcc=false` build.
The author explicitly called it out for a follow-up fix rather than
papering over it inside an unrelated PR.

## Decision

We extend the test's skip predicate to also accept `-ENOSYS` from the
first-frame `vmaf_read_pictures` call.  A new helper
(`hip_submit_one_frame`, extracted to keep `run_hip_motion3` under
the fork's `readability-function-size.LineThreshold=60` budget)
distinguishes the scaffold path from any other failure:

- `err == 0` — proceed to the next frame.
- `err == -ENOSYS` — set `enosys_skip = 1`, tear down the
  partially-initialised `VmafContext`, release the imported
  `VmafHipState`, emit
  `[skip: HIP kernels not built (enable_hipcc=false)]`, and pass.
- any other negative `err` — `mu_assert` fails the test loudly
  (real bug surfaces unchanged).

The CPU baseline is unaffected and the `places=4` tolerance is
unchanged.  When `enable_hip=true && enable_hipcc=true` the test
exercises the real device kernel exactly as before.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Probe `vmaf_hip_available()` from the test (compile-time `HAVE_HIP` proxy) | Single function call, no extractor invocation | Reports the runtime flag, not the kernel-embed flag — fails to distinguish the `enable_hip=true, enable_hipcc=false` posture this fix targets | Does not fix the bug |
| Add a `vmaf_hip_kernels_available()` public API that mirrors `HAVE_HIPCC` | Cleanest separation of concerns | New public surface for a test-only need; bumps the libvmaf SO contract; requires documentation + a corresponding header export | Disproportionate for a test fix |
| Inline `-ENOSYS` detection without extracting a helper | Minimal diff | `run_hip_motion3` grows from 49 to 64 lines, tripping `readability-function-size.LineThreshold=60` in `.clang-tidy` | Per CLAUDE.md §12 rule 12, refactor before NOLINT; helper keeps both functions under the budget |
| Lower the `places=4` tolerance to accommodate the no-kernel path | Trivial diff | Hides the real bug class the test exists to catch (boundary-condition drift in the motion3 moving-average); the user direction was explicit ("never weaken tolerance or skip-without-reason") | Explicitly forbidden by the task brief |

## Consequences

- **Positive**: `meson test --suite gpu` is green on every dev box and
  fork CI runner that ships `enable_hip=true` without
  `enable_hipcc=true`.  Hard parity failures still surface loudly when
  the kernels *are* compiled — only the scaffold path is silenced.
- **Negative**: The test now passes in two distinct
  "no real comparison performed" states (no HIP device, no HIP
  kernels).  A reader scanning the suite output sees the
  `[skip: ...]` marker, but `meson test` summary still counts it as
  a pass, which can mask suite-wide HIP regressions.  The
  `test_hip_smoke.c::test_state_init_runtime_contract` test already
  pins the runtime-vs-scaffold contract independently, so this is
  acceptable.
- **Neutral / follow-ups**: A separate parity regression remains on
  the gfx1036 iGPU when `enable_hipcc=true`
  (`delta=2.91e-03 > 1e-4`).  ADR-0688 documents a related wave32
  carry-loss bug class; the residual motion3 drift is a candidate
  follow-up but is out of scope for the audit-flagged
  `vmaf_read_pictures failed` failure this ADR addresses.
  `test_hip_adm_parity.c` has the symmetric "same-feature-name
  on both sides" bug (`run_extractor("adm", ...)` for both branches
  rather than `"adm_hip"` for the HIP branch); also out of scope
  here.

## References

- PR #443 audit note: pre-existing `test_hip_motion3_parity` failure —
  `HIP: vmaf_read_pictures failed`.
- [`core/src/feature/hip/integer_motion_hip.c`](../../core/src/feature/hip/integer_motion_hip.c)
  lines 442-553 (`init_fex_hip` and `submit_fex_hip` `-ENOSYS`
  scaffold path).
- [`core/src/meson.build`](../../core/src/meson.build) lines 128-166
  (`is_hipcc_enabled` gate, `HAVE_HIPCC` propagation).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend tolerance gate.
- [ADR-0219](0219-motion3-gpu-coverage.md) — motion3
  moving-average post-process.
- [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md) —
  HIP flag promotion + selectable dispatch for `motion_hip`.
- [ADR-0688](0688-hip-wave32-vif-motion-fix.md) — related HIP
  wave32 reduction bugs (context for the residual gfx1036 drift).
- Source: `req` — task brief direct quote (paraphrased):
  "Fix the test (gracefully skip if HIP unavailable; OR fix the
  actual bug if the test is wrong).  Never weaken tolerance or
  skip-without-reason."
