<!-- markdownlint-disable MD060 -->
# ADR-0306: `vmaf-tune` coarse-to-fine CRF search

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, automation, vmaf-tune, ffmpeg

## Context

`vmaf-tune corpus` (Phase A of the harness — see ADR-0237) sweeps a
`(preset, crf)` Cartesian grid of encodes for each reference YUV. The
canonical "tell me the smallest CRF that still meets VMAF >= 92" workflow
asks the tool to sweep the entire `0..51` CRF range — 52 encode + score
passes per `(source, preset)`. On a 1080p `--preset medium` clip with one
pass costing ~5 s, that is ~260 s of wall time **per source × preset
pair**, the bulk of which is wasted: the answer is one CRF; the other 51
are throwaway samples.

The fork already has every primitive needed to do better: the per-trial
encoder + scorer is exposed via `iter_rows`, the JSONL row format records
the VMAF score, and the codec adapters expose a `(crf_min, crf_max)`
range. What was missing was an orchestration layer that picks a small set
of CRFs, scores them, and refines locally around the answer.

## Decision

We will ship a 2-pass coarse-to-fine search (`coarse_to_fine_search`) in
`tools/vmaf-tune/src/vmaftune/corpus.py`, exposed through
`vmaf-tune corpus --coarse-to-fine` (opt-in for the existing subcommand)
and a new `vmaf-tune recommend` subcommand that always runs it. The
defaults are `coarse_step=10`, `fine_radius=5`, `fine_step=1`, scanning
`crf_min=10..crf_max=50` — 5 coarse points plus up to 10 fine points
around the best-coarse CRF, for a total of ~15 visited points (vs 52 for
the full grid → 3.46× wall-time speedup). When the highest-CRF coarse
point already meets `--target-vmaf`, refinement is skipped (1-pass
shortcut, ~10× speedup). The CRF axis on x264 is widened from the old
`(15, 40)` "informative window" to the codec's nominal `(0, 51)` so the
search domain matches what the user typed on the CLI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full grid (status quo) | Trivial; complete picture | 52 encodes per source × preset; 3–4× wasted wall time for target-VMAF flows | Wasteful for the recommend workflow which only needs 1 CRF |
| Binary search over `0..51` | ⌈log₂(51)⌉ = 6 encodes; minimum work | Adaptive — harder to mock in tests; less corpus data emitted; struggles with VMAF non-monotonicity at boundary CRFs | More fragile to score-curve noise; user requested coarse-to-fine framing for clarity |
| Bayesian / GP optimiser (e.g. `scikit-optimize`) | Optimal point selection asymptotically | Heavy dependency, flaky with discrete CRF, overkill for 5 points | Way out of scope for Phase A's 0-runtime-deps mandate |
| Adaptive coarse-only (no fine pass) | 5 encodes total | Granularity is 10 CRF units — recommendation can be 5 CRF off the optimum | The fine pass is what makes the answer trustworthy |
| Fixed 15-point grid (every 4th CRF) | Same point count | No early-exit when target is met cheaply; wastes the boundary cases | The 1-pass shortcut alone is 10× faster on common targets |

## Consequences

- **Positive**:
  - 3.46× fewer encodes for typical `recommend` runs (15 vs 52).
  - Up to 10.4× faster (5 encodes) when the target is met by the highest
    coarse CRF (very common for soft targets like VMAF >= 70).
  - New `recommend` subcommand exposes the canonical workflow without
    forcing scripts to post-process JSONL.
  - `corpus --coarse-to-fine` keeps the JSONL contract — downstream
    Phase B/C consumers work unchanged.
- **Negative**:
  - One more flag surface to keep documented; one more code path to
    maintain in `corpus.py`.
  - The widened x264 quality range means very-low-quality (`crf > 40`)
    encodes are now allowed; for users running scripted full-grid
    sweeps without `--coarse-to-fine`, that means they could probe
    visually unusable CRFs. Acceptable: the CLI is explicit and the
    user opts into the range via `--crf`.
- **Neutral / follow-ups**:
  - Phase B (target-VMAF bisect, ADR-0237) can build on this: the
    recommend subcommand is effectively a Phase B preview.
  - The fine-pass radius/step are exposed as flags so non-x264 codecs
    (h.265 CRF range 0..51, av1 0..63) can swap defaults without code
    changes.

## References

- Parent: [ADR-0237](0237-quality-aware-encode-automation.md) —
  quality-aware encode automation roadmap.
- Source: `req` — user request: paraphrased "add a coarse-to-fine grid
  mode to vmaf-tune; ~3.5× speedup with negligible quality loss; the
  recommend subcommand auto-picks coarse-to-fine when --target-vmaf is
  given since you only need to find the smallest CRF >= target."
