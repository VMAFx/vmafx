<!-- markdownlint-disable MD013 MD060 -->
# ADR-0593: HIP integer_moment kernel — register real HSACO blob alongside psnr / psnr_hvs

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: hip, gpu, parity, build

## Context

Three HIP feature extractors — `integer_psnr_hip`, `integer_psnr_hvs_hip`,
and `integer_moment_hip` — each ship a host translation unit that loads a
kernel module via `hipModuleLoadData(..., <name>_hsaco)`.  Prior PRs landed
the real `.hip` kernel sources for all three under
`core/src/feature/hip/integer_{psnr,psnr_hvs,moment}/*.hip`, and the
meson `hip_kernel_sources` dict already pointed at the `integer_psnr` and
`integer_psnr_hvs` files.

However the `integer_moment` kernel — referenced by `integer_moment_hip.c`
as `integer_moment_score_hsaco` — was not registered: the meson key
`moment_score` exists but resolves to `hip/float_moment/moment_score.hip`
(the float twin), which generates the different symbol `moment_score_hsaco`.
Without an `integer_moment_score` registration the `enable_hipcc=true`
build emitted an unresolved-symbol link error for
`integer_moment_score_hsaco`, and no weak stub covered it (the existing
`hip_hsaco_stubs.c` only stubs the four ADM modules per ADR-0536).

User direction (per request): real kernels everywhere — no stubs for any
of psnr / psnr_hvs / integer_moment.

## Decision

Add a new `integer_moment_score` entry to `hip_kernel_sources` in
`core/src/meson.build` pointing at the existing real kernel source
`feature/hip/integer_moment/moment_score.hip`.  The custom_target emits
`integer_moment_score_hsaco.c` whose symbol satisfies the
`integer_moment_hip.c` reference.  The `moment_score` key for float_moment
is preserved verbatim.

`hip_hsaco_stubs.c` already does **not** carry `psnr_score_hsaco` or
`integer_moment_score_hsaco`, so no stub removal is required — all three
of psnr / psnr_hvs / integer_moment now resolve via the real
xxd-embedded HSACO blob.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add a weak stub for `integer_moment_score_hsaco` in `hip_hsaco_stubs.c` | One-line patch; preserves option to defer kernel work | Runtime returns -ENOSYS, no parity with CPU; user explicitly ruled it out | Rejected by user direction |
| Rename meson key `moment_score` → `integer_moment_score` and repoint to integer kernel | Single key matches host symbol exactly | Breaks the float_moment host link (`float_moment_hip.c` consumes `moment_score_hsaco`) | Would silently regress float_moment |
| **Add new `integer_moment_score` entry alongside `moment_score`** | Both extractors keep their real HSACO blob; host symbols match emit names; no stub anywhere | Two near-identical keys require a clarifying inline comment | **Chosen** — fully verified parity, documented inline |

## Consequences

- **Positive**: `integer_moment` HIP path now matches CPU bit-exactly
  (verified: `float_moment_ref1st/dis1st/ref2nd/dis2nd` delta = 0.000000
  on the Netflix `src01_hrc00 ↔ src01_hrc01` 576x324 pair).  Same for
  `psnr_y/cb/cr` and `psnr_hvs / psnr_hvs_y/cb/cr` (delta = 0.000000).
- **Negative**: dual `moment_score` / `integer_moment_score` keys in
  the meson dict require a clarifying inline comment so future
  contributors don't fold them.
- **Neutral / follow-ups**: future audits of `hip_kernel_sources`
  should treat the inline-comment-distinguished pair as
  semantically-distinct entry points.

## References

- ADR-0533: integer ADM HIP dispatch (sibling kernel registration).
- ADR-0536: weak HSACO stubs for ADM modules that still reference CUDA
  helper macros.
- Source: `req` — user request: port the three HIP kernels and replace
  weak HSACO stubs with real kernel code; no stubs anywhere.
- Verify command in PR description; per-feature delta tabulated in the
  deep-dive digest at
  [`docs/research/hip-integer-moment-registration.md`](../research/hip-integer-moment-registration.md).
