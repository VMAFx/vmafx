<!-- markdownlint-disable MD013 MD060 -->

# ADR-1193: Opt-in `uncapped` option splits the PSNR infinity sentinel from the truncation

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: core, feature, psnr, options, cuda, sycl, hip, metal, gpu-twins, upstream

## Context

Both PSNR extractors clamp every frame's score at a per-bit-depth ceiling
`psnr_max` (`(6 * bpc) + 12` for the integer extractor, a 60/72/84/108 dB
table for `float_psnr`). The clamp was written to solve one problem — when
the reference and distorted planes are byte-identical the SSE is zero, the
true PSNR is `+inf`, and something finite has to be reported — but it was
applied unconditionally, so it also silently truncated every *genuinely
computed* value above the ceiling.

The consequence is a wrong number, not a missing one. An 8-bit 576x324 pair
differing by a single luma step has SSE 1 over 186624 samples and a true
PSNR of 100.840479 dB; the shipped binary reported `psnr_y = 60.000000`,
which is also what a caller comparing against FFmpeg's `psnr` filter (which
reports 100.840479 on the same pair) sees as an unexplained 40 dB gap. The
same expression is replicated across the eight GPU twins, so no backend
escaped it. Reported upstream as Netflix/vmaf#1109 and tracked here as
`T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03`.

An escape hatch already existed — `--feature psnr=min_sse=<eps>` raises the
ceiling — but it is undocumented, it is integer-extractor-only, and it
raises the *sentinel* along with the ceiling, so identical planes start
reporting 155 dB instead of 60 dB. That trades one wrong number for another.

The constraint that shapes the decision is the Netflix golden gate: the
60 / 84 / 108 dB assertions in `python/test/` are all byte-identical
(`sse == 0`) pairs, i.e. they pin the *sentinel*, not the truncation. Any
fix must leave them bit-identical, which rules out simply removing the
clamp.

## Decision

We will add an opt-in boolean feature-extractor option `uncapped` (default
`false`) to the `psnr` and `float_psnr` extractors and to all eight GPU
twins, and split `psnr_max`'s two roles: on the `uncapped` path the
`mse == 0` case reports `psnr_max` as the infinity sentinel and nothing else
is truncated, while the default path keeps the shipped expression
character-for-character rather than re-deriving it. Keeping it verbatim is
what makes the default bit-identical *by construction* — a re-derived
`mse == 0 -> psnr_max` default would differ wherever `min_sse` pushes the
ceiling past the ~208 dB a floored zero MSE produces. The option is deliberately *not*
flagged `VMAF_OPT_FLAG_FEATURE_PARAM`, so feature names (`psnr_y`,
`float_psnr`, …) are unchanged whether or not it is set.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Opt-in `uncapped` option (chosen)** | Default scores bit-identical, so the golden gate and every published snapshot are untouched; the sentinel keeps its documented value; one name and one default across CPU and all eight GPU twins | A new stable option surface to maintain on ten extractors; users must know to ask for it | — |
| Remove the truncation outright (report the true value always) | Simplest code; matches FFmpeg and every other PSNR implementation | Moves every fork-added GPU/SIMD snapshot and any downstream consumer that pooled a capped score; a behaviour change on a stable metric with no opt-out | Rejected: an unannounced score move on the fork's most-consumed feature. Revisit only behind a major-version ADR. |
| Document `min_sse` as the supported escape hatch and change nothing else | Zero code change | Inflates the `sse == 0` sentinel (155 dB instead of 60 dB on identical planes), is integer-only, and requires the caller to pick an epsilon that depends on the frame area | Rejected: replaces one wrong number with another, and cannot express "true value, sentinel unchanged" at all. |
| Emit `+inf` for `sse == 0` and never clamp | Mathematically honest | `inf` is not representable in the JSON/XML/CSV output schemas, breaks pooling (`mean` of `inf`), and moves the golden 60/84/108 dB assertions | Rejected: breaks the output contract and the golden gate simultaneously. |
| Make `uncapped` a `VMAF_OPT_FLAG_FEATURE_PARAM` so scores land under a suffixed name | A capped and an uncapped run could coexist in one output | The GPU twins would emit `psnr_y_uncapped` while the CPU emits `psnr_y` (the CPU extractor appends without a name dict), i.e. a new cross-backend divergence; and callers would have to rename their parsers | Rejected: the option changes the value of an existing feature, not its identity. |

## Consequences

- **Positive**: `--feature psnr=uncapped=true` (and `float_psnr=uncapped=true`)
  reports the true PSNR, matching FFmpeg's `psnr` filter, on every backend
  with one option name. The `mse == 0` sentinel now has exactly one meaning
  in both modes, and the previously-undocumented `min_sse` escape hatch is
  documented alongside it in a new `docs/metrics/psnr.md`.
- **Negative**: ten extractors now carry the option and its branch, and the
  branch has to stay mirrored on backends that cannot be exercised on the
  fork's own hardware (Metal). The default remaining capped means a caller
  who does not read the docs still gets a truncated number.
- **Neutral / follow-ups**: no snapshot regeneration is needed — the default
  is bit-identical, which `core/test/test_psnr_uncapped.c` pins from both
  directions. The GPU twins' remaining option-table divergence from the CPU
  extractor (`enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` are
  still CPU-only) is untouched by this ADR and stays recorded in
  `core/AGENTS.md`.

## References

- Netflix/vmaf#1109 — "PSNR value is capped".
- `docs/state.md` — `T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03`.
- [ADR-1166](1166-upstream-issue-harvest.md) — the harvest that verified this report against the fork.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — prior art for mirroring a CPU option table onto the GPU twins.
- `docs/metrics/psnr.md` — the user-facing documentation this ADR requires.
