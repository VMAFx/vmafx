<!-- markdownlint-disable MD013 MD060 -->
# ADR-0006: Set CLI precision default to %.17g with --precision flag

- **Status**: Superseded by [ADR-0119](0119-cli-precision-default-revert.md)
  (2026-04-19)
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: cli, testing, python

## Context

Upstream VMAF printed scores with a fixed short format and used a `count_leading_zeros_d` heuristic in `output.c` to trim trailing zeros. This truncated digits silently and broke exact-match comparisons between the C CLI output and the Python test suite. For cross-backend numeric diffing (CPU vs SIMD vs CUDA vs SYCL) to work, the printed representation has to round-trip losslessly to the underlying `double`.

## Decision

We will make the CLI precision default `%.17g` (the shortest IEEE-754 round-trip-lossless representation), add a `--precision=N` flag, accept `--precision=max` as an alias for `%.17g`, apply the format to both stderr and file output (XML/JSON/CSV/sub-XML), drop the `count_leading_zeros_d` heuristic in `output.c`, and propagate the same change to the Python side (`result.py:119,132`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `%.10f` fixed default | Human-readable; matches some legacy expectations | Truncates in edge cases; still breaks comparisons | Does not round-trip losslessly |
| Keep `count_leading_zeros_d` heuristic | No visible change | Silent truncation; opaque behaviour | Actively harmful for cross-backend diff |
| `%.17g` default (chosen) | IEEE-guaranteed round-trip; identical doubles produce identical strings; enables exact diffing | Longer strings | Mathematically correct answer |

Rationale note: `%.17g` is the IEEE-guaranteed shortest representation that round-trips losslessly — any numerically equal doubles produce bit-identical strings, enabling exact diffing across backends. Fixed `%.10f` would still truncate in some cases.

## Consequences

- **Positive**: CPU vs SIMD vs GPU score comparisons become exact string diffs; Python test suite comparisons stop flaking.
- **Negative**: stderr output is visually longer; CSV columns widen.
- **Neutral / follow-ups**: update snapshot JSONs via ADR-0009 flow; Python `result.py` and C `output.c` must stay in lockstep.

## References

- Source: `Q2.2`, `Q3.1`
- Related ADRs: ADR-0009, ADR-0024
