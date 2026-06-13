# ADR-0176: Vulkan VIF cross-backend gate (lavapipe + Arc nightly)

- **Status**: Accepted (errata 2026-04-26 below — body unchanged per ADR-0028)
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ci`, `vulkan`, `gpu`, `numerical-correctness`

> **Errata (2026-04-26)** — the "ULP=0" empirical baseline asserted in
> the original body of this ADR was bogus. The cross-backend gate
> shipped here was silently exercising **CPU on both sides** because
> three latent bugs collapsed onto each other:
> (1) `tools/meson.build` never set `-DHAVE_VULKAN=1`, so every
> `--vulkan_device` invocation no-op'd and the binary ran CPU;
> (2) `vmaf_use_feature()` skipped `set_fex_vulkan_state()`, so when
> the script DID select the Vulkan extractor by name, the imported
> state never reached it and the lazy fallback auto-picked NVIDIA
> RTX 4090 instead of the requested device; (3) the script invoked
> `--feature X` for both CPU and "Vulkan" runs alongside the
> auto-loaded VMAF model, racing CPU and GPU extractors on the same
> output names — second writer's score silently dropped.
>
> All three are fixed in commits `6167e300`, `de65c0ac`, `2aa79db1`,
> `05d5a7f5` (PR #120). The corrected gate's actual baseline against
> the CPU scalar reference, on Intel Arc A380 via Mesa anv, is
> **places=4 clean with max_abs ≤ 1e-6 (essentially JSON %f
> precision noise)** for `vif_scale0..3`. Other GPU backends have
> their own residuals — see ADR-0178 § "Bug history" for the full
> matrix and the kernel-side follow-ups.

## Context

PR #117 (commit `acf9f5b8`) merged the four-scale Vulkan VIF GLSL kernel,
ported from the SYCL reference implementation. The kernel is registered as
the `vif_vulkan` feature extractor and produces the standard
`VMAF_integer_feature_vif_scale0..3_score` outputs on Intel Arc A380.

Before extending the Vulkan path to ADM / motion / motion_v2 (T5-1c) we
need a CI gate that locks in numerical agreement between the Vulkan kernel
and the CPU scalar reference. Without one, future kernel work can drift
silently — the user's standing rule (CLAUDE.md §8) is that the CPU integer
extractors are the numerical-correctness ground truth.

Two runner options were considered for the gate. GitHub-hosted runners do
not expose any GPU, so a real-hardware Vulkan ICD requires self-hosted
infrastructure. Mesa's `lavapipe` software ICD provides Vulkan 1.3 on stock
`ubuntu-24.04` runners with no GPU — slower than hardware, but anyone can
rerun a failure.

## Decision

We will gate the Vulkan VIF kernel with two complementary cross-backend
diff lanes:

1. **`Vulkan VIF Cross-Backend (lavapipe, places=4)`** — runs on every PR
   on `ubuntu-24.04` using Mesa lavapipe via `VK_LOADER_DRIVERS_SELECT`.
   Required status check.
2. **`Vulkan VIF Cross-Backend (Arc A380, advisory)`** — runs nightly on
   the self-hosted Arc runner (label `vmaf-arc`). Advisory; catches
   lavapipe-vs-real-driver divergence. Parked behind `if: false` until
   the self-hosted runner is registered, at which point it flips on.

Both lanes invoke `scripts/ci/cross_backend_vif_diff.py`, which compares
per-frame `integer_vif_scale0..3` scores at `places=4` (round-half-even).
The Netflix normal pair (`src01_hrc{00,01}_576x324.yuv`) is the fixture.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Lavapipe only | Free hosted runner; reproducible | Software emulation may diverge from a real ICD on subtle rounding cases | Hardware lane catches drift the lavapipe lane cannot |
| Self-hosted Arc only | Fastest, real-driver semantics | Every PR depends on private hardware availability; rerunning failed jobs requires runner uptime | Excludes external contributors from running the gate |
| `places=4` snapshot vs CPU | Matches fork norm — GPU is not bit-exact with CPU in general | Slack hides regressions tighter than 4 decimals | Adopted; ULP-strict gate would have to track future driver changes |
| ULP ≤ 2 vs SYCL | Both GPU; reduction patterns similar | Doesn't validate Vulkan against the spec, only against another GPU | Doesn't catch drift in either kernel |

The empirical baseline as of `acf9f5b8` is **ULP = 0** between Vulkan-on-Arc
and the CPU scalar reference (the GLSL kernel uses native `int64`
accumulators via `GL_EXT_shader_explicit_arithmetic_types_int64`). The
`places=4` slack is preserved for forward compatibility — if a future Mesa
driver, Khronos spec change, or kernel-side optimization changes the bit
pattern, the gate still passes as long as the score agrees to four
decimals.

## Consequences

- **Positive**: the Vulkan kernel matrix can grow (T5-1c: ADM, motion,
  motion_v2) without silent numerical regression. Lavapipe makes the gate
  reproducible by anyone with a stock Ubuntu runner.
- **Negative**: lavapipe is single-threaded software emulation; the lane
  takes longer than a real-driver run. The Arc nightly lane requires
  registering and maintaining a self-hosted runner.
- **Neutral / follow-ups**:
  - The existing `Cross-Backend ULP Diff (CPU Sanity)` lane stays parked;
    its scope is broader (CUDA/SYCL/CPU snapshot) and waits on its own
    runner story.
  - Bit-exactness of the GLSL kernel should be retested when:
    (a) new Vulkan kernels land for ADM / motion;
    (b) Mesa Vulkan driver bumps a major version;
    (c) any SCALE-0 or SCALE-1+ filter coefficient changes upstream.
  - The Arc nightly lane is parked at `if: false` until a self-hosted
    runner is registered with label `vmaf-arc`. Flipping it on is a
    follow-up PR (no code change required).

## References

- ADR-0127 — Vulkan backend decision (Q2 governance).
- ADR-0175 — Vulkan backend scaffold (T5-1).
- PR #117 — T5-1b-iv VIF math port (commit `acf9f5b8`).
- CLAUDE.md §8 — Netflix golden-data gate (CPU is the ground truth).
- `req` — user direction 2026-04-25:
  *paraphrased:* run lavapipe in CI as the blocking gate and the Arc
  self-hosted runner nightly as advisory; tolerance is `places=4` against
  the CPU scalar reference.
