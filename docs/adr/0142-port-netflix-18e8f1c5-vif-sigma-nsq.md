# ADR-0142: Port Netflix upstream `vif_sigma_nsq` feature parameter

- **Status**: Accepted
- **Date**: 2026-04-22
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: upstream-port, feature-param, vif, simd

## Context

Netflix upstream commit
[`18e8f1c5`](https://github.com/Netflix/vmaf/commit/18e8f1c5)
(2026-04-20) promotes VIF's hard-coded neural-noise variance
`static const float sigma_nsq = 2` into a runtime-configurable
feature parameter `vif_sigma_nsq`, threaded through
`compute_vif` → `vif_statistic_s` with default `2.0` preserving
existing scores. Purpose: let downstream model-training pipelines
sweep the parameter without recompiling. The upstream commit also
derives `sigma_max_inv = pow(vif_sigma_nsq, 2) / 255²` so the
masked-region score stays self-consistent at non-default values.

The fork differs from upstream here in one structural way: a
fork-local AVX2 SIMD variant `vif_statistic_s_avx2` under
`core/src/feature/x86/vif_statistic_avx2.c` that upstream does
not ship. Upstream's commit touches only the scalar path and the
scalar-facing headers. A plain cherry-pick leaves the AVX2 path
with the old two-argument signature, breaks the call-site link in
`vif_tools.c`, and silently loses the parameter when AVX2 is
selected at runtime.

## Decision

We will port upstream `18e8f1c5` and extend the AVX2 SIMD variant
to accept `vif_sigma_nsq` as a 14th parameter, so scalar and AVX2
agree on the new contract. Rules:

1. Default-path bit-identity — when `vif_sigma_nsq = 2.0`, both
   paths must produce scores that are bit-identical to pre-port
   master. The AVX2 body keeps its local `sigma_nsq` float and
   `sigma_max_inv` float constants; `sigma_max_inv =
   powf((float)vif_sigma_nsq, 2.0f) / (255.0f * 255.0f)` evaluates
   to exactly `4.0f / 65025.0f` at default, matching the
   pre-port `static const float sigma_max_inv = 4.0 / (255.0 *
   255.0)`.
2. Float discipline — unlike upstream's scalar body (which inlines
   the `double vif_sigma_nsq` parameter directly into float
   arithmetic and implicitly double-promotes `sv_sq +
   vif_sigma_nsq`), the fork's scalar path keeps
   `const float sigma_nsq = (float)vif_sigma_nsq;` and uses the
   float-cast local in compute sites. This preserves the fork's
   ADR-0138 / ADR-0139 float-arithmetic invariant and avoids an
   unintended precision shift at the default value.
3. Upstream parity for header signatures — `vif.h`,
   `vif_tools.h`, `vif_tools.c`, `float_vif.c`, and the python
   wiring in `feature_extractor.py` adopt upstream's shapes
   verbatim, with the fork's pre-existing indentation /
   quote-style preserved.
4. ADR-0141 discipline on touched files — the stride→pointer-offset
   widening warnings in `vif_statistic_avx2.c` are fixed in-place
   by casting `*_stride` to `ptrdiff_t` during px-stride division.
   The `readability-function-size` warning on
   `vif_statistic_s_avx2` is explicitly NOLINT'd with a reference
   to ADR-0141 §Historical debt and backlog item T7-5 — the
   function pre-dates the touched-file cleanup rule and splitting
   it would entangle ADR-0138 / ADR-0139 bit-exactness invariants
   across new helpers without a net audit-ability gain.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Port + extend AVX2 with fork's float-discipline (chosen)** | Default scores bit-identical to pre-port; AVX2 and scalar agree on new contract; non-default values behave sensibly; on-policy for ADR-0138 / ADR-0139 / ADR-0141 | Small divergence from upstream at non-default values (upstream double-promotes in scalar; fork stays float) | **Decision** — default-path bit-identity is the fork's primary invariant |
| Port with double-promotion matching upstream verbatim | Zero fork divergence | Drifts default-path scores by small ULP amounts because `sv_sq + (double)vif_sigma_nsq` is a different computation from the pre-port `sv_sq + sigma_nsq_float` | Rejected — breaks the fork's bit-identity invariant on default |
| Defer port (stay on hard-coded `sigma_nsq = 2`) | Simplest; no diff | Blocks downstream model-training sweeps; upstream divergence grows | Rejected — routine upstream catch-up |
| Port scalar only, leave AVX2 silently dropping the parameter | Smaller diff | Silent correctness bug at runtime when AVX2 is selected and user passes non-default `snsq` | Rejected — unacceptable |

## Consequences

- **Positive**:
  - Downstream model-training can sweep `vif_sigma_nsq` via CLI
    (`--feature float_vif:snsq=X`) without a recompile.
  - Fork + AVX2 paths now agree on the new 14-parameter contract.
  - Default-path scores bit-identical to pre-port master.
- **Negative**:
  - One new feature parameter to document and gate in tests.
  - AVX2 path now references `vif_sigma_nsq` explicitly; next
    upstream AVX2 rewrite (if any) will carry this into a rebase.
- **Neutral / follow-ups**:
  - `docs/rebase-notes.md` entry 0035 records workstream,
    touched files, the default-path invariant, and the
    fork-vs-upstream float-discipline divergence.
  - `CHANGELOG.md` entry under Unreleased → Added.
  - `core/src/feature/AGENTS.md` gains a rebase-sensitive
    note about the AVX2 variant's new parameter.
  - Python tests for the new parameter inherited verbatim from
    upstream (`feature_extractor_test.py`,
    `vmafexec_feature_extractor_test.py`).

## References

- Upstream commit:
  [Netflix/vmaf@18e8f1c5](https://github.com/Netflix/vmaf/commit/18e8f1c5)
  "feature/vif: add vif_sigma_nsq" (Kyle Swanson, 2026-04-20).
- Related ADRs:
  [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) /
  [ADR-0139](0139-ssim-simd-bitexact-double.md) — float-arithmetic
  discipline for SIMD on `_iqa_*`;
  [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  cleanup rule applied to the AVX2 variant.
- Source: user direction 2026-04-22 ("manually integrate latest
  commits on upstream") + post-audit popup confirming the port
  order (vif_sigma_nsq first, simplest + additive).
