<!-- markdownlint-disable MD013 -->
# ADR-0143: Port Netflix upstream generalised AVX convolve for arbitrary filter widths

- **Status**: Accepted
- **Date**: 2026-04-22
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: upstream-port, simd, avx2, vif, convolve

## Context

Netflix upstream commit
[`f3a628b4`](https://github.com/Netflix/vmaf/commit/f3a628b4)
(2026-04-21) replaces four large specialised AVX convolve kernels
in `core/src/feature/common/convolution_avx.c` (each hard-coded
to a specific filter width ∈ {3, 5, 9, 17}) with a single
generalised pair of 1-D scanline routines. The old file was 2,747
lines of branch-unrolled code; the new one is 247 lines. The
generalised path is gated by `MAX_FWIDTH_AVX_CONV` (new constant
in `convolution.h`), and the VIF dispatch in `vif_tools.c` drops
the hard-coded `fwidth == 17 || ... == 3` whitelist in favour of
`fwidth <= MAX_FWIDTH_AVX_CONV`.

This is distinct from the fork's ADR-0138 work on `iqa/convolve.c`
(which is a different upstream file owned by the IQA stack). The
fork has no local changes to `convolution_avx.c` / `convolution.h`
beyond CI-generated formatting — a plain cherry-pick drops
cleanly into the fork.

Netflix ships a paired python-test update: the two Netflix
assertions that compare full VMAF scores at the integer level
(`VMAF_score = 88.033…` and `VMAFEXEC_score = 88.4895`) loosen
from `places=2` (±0.005) to `places=1` (±0.05). The generalised
kernel's accumulation order differs at ULP scale from the
specialised ones, producing a small final-VMAF drift that's well
below both integer-scale quality discrimination and the model's
training noise floor. Netflix's own upstream authority is the
tie-breaker here: the fork follows their loosening (same pattern
as ADR-0142 for `vif_sigma_nsq`).

## Decision

We will port upstream `f3a628b4` verbatim for `convolution_avx.c`,
`convolution.h`, the VIF dispatch in `vif_tools.c`, and the two
paired python-test assertions. Additionally, per ADR-0141, the
generalised `convolution_avx.c` is left lint-clean:

1. **Internal linkage on helpers** — the four `1d_*_scanline`
   helpers are fork-local `static` (no other TU references them
   after the specialised-path removal; upstream leaves them with
   external linkage out of habit).
2. **`ptrdiff_t` stride type** — strides widen to `ptrdiff_t` in
   the static helpers, and public wrappers cast at the
   multiplication sites. Eliminates the
   `bugprone-implicit-widening-of-multiplication-result` warning
   on every `k * src_stride` pointer-offset form.
3. **`#include <stddef.h>`** — required for `ptrdiff_t`.
4. **Netflix golden loosening** — the two `VMAF_score` /
   `VMAFEXEC_score` assertions in
   `python/test/quality_runner_test.py:1997` and
   `python/test/vmafexec_test.py:1274` move from `places=2` to
   `places=1`, matching upstream. This is a Netflix-authorised
   change to a Netflix-owned test; project rule #1 ("never modify
   Netflix golden assertions") addresses fork drift, not
   upstream-authored loosenings that the fork must track. Same
   shape as ADR-0142.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Port verbatim + ADR-0141 cleanup + adopt upstream python loosening (chosen)** | On-policy for every fork rule; unlocks `fwidth <= MAX_FWIDTH_AVX_CONV` parametric use; drops 2.4k LoC of specialised branches; stays aligned with upstream | Python assertion loosening is a sensitive operation; needs the ADR-0142 / Netflix-authority precedent cited | **Decision** — routine upstream catch-up, matches the established precedent |
| Port C changes only, keep fork's `places=2` assertions | Tighter numerical invariant | Netflix CPU golden check would go red because upstream's own generalised kernel doesn't hit those asserts at `places=2`; permanent CI failure | Rejected — same-file inconsistency is worse than the tolerance loosening |
| Keep specialised kernels, cherry-pick only the `MAX_FWIDTH_AVX_CONV` constant + dispatch relaxation | Zero change to scalar numerics | Specialised kernels don't exist for arbitrary fwidth, so the relaxed dispatch wouldn't work; defeats the point | Rejected — constants without the implementation is dead code |
| Keep upstream's loose `int` stride types; NOLINT the widening warnings | Smaller diff | ADR-0141 requires touched-file cleanup; `ptrdiff_t` is the right type and the refactor is local | Rejected — ADR-0141 touched-file rule applies |

## Consequences

- **Positive**:
  - VIF / VIF-like kernels can now dispatch AVX for any filter
    width `<= MAX_FWIDTH_AVX_CONV`, not just the four
    specialised widths. Enables downstream kernelscale
    experimentation without adding another specialised kernel.
  - `convolution_avx.c` drops from 2,747 LoC to 247 LoC — one
    scanline impl instead of four-way branch-unrolled copies.
  - Touched file is fully lint-clean (zero clang-tidy warnings).
- **Negative**:
  - Per-frame perf may drop slightly vs the hand-unrolled
    specialised kernels for fwidth ∈ {3, 5, 9, 17}. Upstream
    accepted this trade-off; if a profiling pass shows a hot
    spot, a specialised-path fast path could be reintroduced as
    a follow-up.
  - Two Netflix golden assertions loosened by 10× (±0.005 →
    ±0.05) at the VMAF-score level. Below perceptual
    discriminability; documented here.
- **Neutral / follow-ups**:
  - `docs/rebase-notes.md` entry 0036 records the fork vs
    upstream divergence on the static-linkage + `ptrdiff_t`
    cleanup.
  - `CHANGELOG.md` entry under Unreleased → Changed.
  - `core/src/feature/AGENTS.md` gains a rebase-sensitive
    note about the static helpers and `ptrdiff_t` strides.

## References

- Upstream commit:
  [Netflix/vmaf@f3a628b4](https://github.com/Netflix/vmaf/commit/f3a628b4)
  "feature/common: generalize avx convolution for arbitrary
  filter widths" (Kyle Swanson, 2026-04-21).
- Related ADRs:
  [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — the
  IQA-stack companion (different file, different discipline);
  [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  cleanup rule applied here;
  [ADR-0142](0142-port-netflix-18e8f1c5-vif-sigma-nsq.md) —
  precedent for adopting upstream-authored python-golden
  loosenings.
- Source: user direction 2026-04-22 ("manually integrate latest
  commits on upstream"), port-order confirmation on the audit
  popup.
