<!-- markdownlint-disable MD013 MD025 MD033 MD060 -->
<!-- ADR-0665 stub — claimed by scripts/adr/next-free.sh --claim fast-nr-calibration-quality-guard on 2026-05-21T16:51:54Z -->
<!-- Replace this file with the real ADR and rename to 0665-fast-nr-calibration-quality-guard.md before committing. -->
<!-- To abandon this claim run: scripts/adr/next-free.sh --release 0665 -->

# ADR-0665: <fill in title>

- **Status**: Proposed
- **Date**: 2026-05-21
- **Deciders**: <fill in>
- **Tags**: <fill in>

## Context

<fill in>

## Decision

<fill in>

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| | | | |

## Consequences

- **Positive**: <fill in>
- **Negative**: <fill in>
- **Neutral / follow-ups**: <fill in>

## References

- See [ADR-0535](0535-adr-atomic-allocator.md) for the original allocator design.
- See [ADR-0628](0628-adr-allocator-remote-aware.md) for the remote-aware extension.
- Source: <req or Q<round>.<q>>

# ADR-0665: Fast-NR calibration quality guard

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris maintainers
- **Tags**: ai, vmaf-tune, calibration, fast-nr, quality-gate

## Context

`vmaf-tune --fast-nr` uses `nr_metric_v1` as a cheap no-reference proxy before
paying for full-reference VMAF. The proxy only unlocks safe consumer-hardware
speedups when its sidecar maps raw NR scores into VMAF units with a meaningful
corpus fit. A 2026-05-21 CPU calibration sweep over the current local corpus
produced a very weak fit (`PLCC=0.0821`, `sigma=16.2445`, `delta_fast=32.49`),
which would turn the sidecar into a misleading runtime contract if written.

## Decision

`ai/scripts/calibrate_nr_threshold.py` will quality-gate every attempted JSON
sidecar write. The default gate requires at least 10 calibration samples and
`PLCC >= 0.70`. Weak fits still produce a Markdown report, but the script
refuses to update `nr_metric_v1.json` unless the operator explicitly passes
`--allow-weak-calibration`. The sidecar records gate status, gate thresholds,
and rejection reasons whenever it is written.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep writing every fitted sidecar | Fastest to operate; preserves old CLI behavior | Can promote non-predictive NR fits into `vmaf-tune` early-elimination and hide the reason inside a JSON sidecar | The measured PLCC failure showed this is unsafe for the Netflix-style consumer pipeline |
| Use only sample-count gating | Blocks tiny smoke fits | Still accepts anti-correlated or flat NR signals when enough rows exist | The real failed sweep had enough rows; correlation was the missing safety check |
| Require manual review outside the script | Keeps code simple | Easy to skip during overnight training/recalibration runs; no machine-readable status reaches the sidecar | The guard belongs at the write boundary that creates the tune input |

## Consequences

- **Positive**: `vmaf-tune` only consumes fast-NR sidecars that passed a
  minimum predictive-signal check, making the fast path a real speedup rather
  than a hidden quality risk.
- **Negative**: quick one-clip calibration probes now fail the write gate unless
  callers lower the sample gate or use `--allow-weak-calibration` for
  diagnostics.
- **Neutral / follow-ups**: future NR models should tune the default PLCC gate
  with held-out real-corpus evidence instead of weakening tests or runtime
  thresholds.

## References

- [Research-0685](../research/0685-fast-nr-calibration-quality-guard.md)
- [ADR-0615](0615-fast-nr-prescoring.md)
- [ADR-0624](0624-fast-nr-prescoring-impl.md)
- Source: `req` — "ai unlocks the speedup in tune for the full netflix style pipeline on consumer stuff"
