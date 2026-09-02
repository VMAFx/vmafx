<!-- markdownlint-disable MD060 -->
# ADR-0656: External-bench wrappers emit registry competitor keys

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, testing, tooling, fork-local

## Context

`tools/external-bench/compare.py` validates each wrapper payload before
aggregation. The validator requires `summary.competitor` to equal the
wrapper registry key from `WRAPPERS`, because that key is what the CLI
accepts through `--competitors` and what the report table groups by.

The two fork-side shell wrappers emitted descriptive labels instead:
`fork-fr-regressor-v2-ensemble` and `fork-nr-metric-v1`. Those labels are
useful as display prose, but they violate the schema introduced by
Research-0118 and cause the fork's own competitors to be skipped before
aggregation. This weakens the external-bench path exactly where the
signal-mix audit needs NR/MOS second-opinion comparisons.

## Decision

The fork-side wrappers will emit the exact registry keys
`fork-fr-regressor` and `fork-nr-metric` in `summary.competitor`. Model and
version detail stays out of that identity field and belongs in optional
metadata, wrapper logs, or future report columns. Shell-wrapper smoke tests
will exercise the two fork wrappers with a fake `vmaf-tune` binary so this
contract is pinned without installing external competitors.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix wrapper output to registry keys | Keeps the documented schema strict; makes table grouping deterministic; minimal user-visible change | Descriptive version labels are no longer shown in the identity column | Chosen. The identity field is a machine contract, not a display label. |
| Relax `validate_wrapper_output()` to accept aliases | Preserves the current descriptive labels | Reopens schema ambiguity and needs alias tables everywhere aggregation compares keys | Rejected. It makes the validator weaker immediately after adding it. |
| Change `WRAPPERS` keys to the descriptive labels | Keeps wrapper output unchanged | Breaks existing `--competitors fork-fr-regressor fork-nr-metric` usage and docs | Rejected. The public CLI keys are already documented. |

## Consequences

- **Positive**: `compare.py` can aggregate the fork-side FR and NR wrappers
  instead of skipping them as invalid schema.
- **Positive**: the tests now exercise the real shell wrappers at the
  schema boundary while still stubbing `vmaf-tune` and external binaries.
- **Negative**: operators who read only the `summary.competitor` field get
  the stable key rather than the exact model version; future metadata can
  carry model IDs explicitly if needed.
- **Neutral / follow-ups**: NR/MOS second-opinion feature materialisation
  should consume the same registry keys when it reuses external-bench
  wrapper output.

## References

- `req`: "well go on i guess we have enough backlog..."
- [ADR-0368](0368-external-bench-wrapper-only.md) — wrapper-only external
  benchmark harness.
- [Research-0118](../research/0118-external-bench-wrapper-schema-validation-2026-05-14.md)
  — schema validation at the wrapper boundary.
- [ADR-0650](0650-signal-mix-audit.md) — signal-mix audit identifies
  NR/MOS second opinions as a missing signal family.
