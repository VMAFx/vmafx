<!-- markdownlint-disable MD060 -->
# ADR-0431: Split CUDA and CPU Feature Passes for FR-from-NR Extraction

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Codex
- **Tags**: ai, cuda, training-data, corpus, fork-local

## Context

`ai/scripts/extract_k150k_features.py` is the local FR-from-NR extractor used
for K150K and ad-hoc CHUG FULL_FEATURES materialisation. CHUG experiments
exposed that the current all-feature `--backend cuda` invocation can fail on
10-bit clips with duplicate feature-key warnings followed by
`context could not be synchronized`.

The CUDA binary does expose twins for most requested FULL_FEATURES, so the
failure is not a missing-feature problem. The unstable path is the generic
feature-name bundle plus backend auto-selection in a single libvmaf context.

## Decision

We will split CUDA extraction into two explicit passes. The first pass uses
explicit CUDA extractor names for the stable CUDA-backed feature set; the
second pass runs residual CPU extractors (`float_ssim`, `cambi`) through
`--cpu-vmaf-bin`; the script merges per-frame metric dictionaries before
aggregation so the parquet schema remains unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep one generic `--backend cuda` invocation | Fastest command shape, no script complexity | Reproduces CHUG failures on 10-bit clips and loses rows | Rejected because the current local HDR run cannot complete reliably |
| Force CPU-only extraction | Stable and already documented | Leaves CUDA hardware idle and makes CHUG/K150K passes much slower | Rejected because explicit CUDA feature names work for most of the bundle |
| Add feature-specific retry after failure | Maximises fallback coverage | Wastes time failing first and makes progress accounting noisy | Rejected in favour of deterministic split routing |

## Consequences

- **Positive**: CHUG and K150K local extraction can use CUDA where it is stable
  without losing the existing 22-feature output schema.
- **Negative**: CUDA mode launches two libvmaf subprocesses per clip and needs
  both `--vmaf-bin` and `--cpu-vmaf-bin` to exist locally.
- **Neutral / follow-ups**: CHUG and K150K feature materialisation should be
  deduplicated behind a shared FR-from-NR helper so backend routing is not tied
  to one corpus-specific script name.

## References

- [ADR-0362](0362-k150k-corpus-integration.md)
- [ADR-0383](0383-k150k-parallel-cpu-driver.md)
- [ADR-0426](0426-chug-hdr-corpus-ingestion.md)
- [ADR-0427](0427-chug-hdr-feature-materialisation.md)
- Source: `req` — "what? those features are in cuda, no?"
- Source: `req` — "well then fix that, no?"
