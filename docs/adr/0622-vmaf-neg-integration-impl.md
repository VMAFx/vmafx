<!-- markdownlint-disable MD060 -->
# ADR-0622: VMAF NEG Integration Implementation

- **Status**: Accepted (Implemented)
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `neg`, `docs`

## Context

ADR-0616 decided to expose VMAF NEG model variants in `vmaf-tune` via a
`--neg` flag. The NEG model files (`model/vmaf_v0.6.1neg.json` and
`model/vmaf_4k_v0.6.1neg.json`) were already present in the repository.
No additional model training was required.

This ADR records the concrete implementation decisions:

1. Where the `neg_model_for()` routing helper lives (`resolution.py`),
   so it is co-located with the related `select_vmaf_model_version()` logic
   and the `MODEL_*` constants.
2. Where the `_add_neg_flag()` helper lives (`cli.py`), so every
   subparser wires the flag identically.
3. Where `_resolve_vmaf_model()` lives (`cli.py`), producing the
   effective model string at runner time rather than parser time —
   this keeps the routing out of argparse callbacks and testable
   as a pure function.
4. How `make_default_sampler()` in `ladder.py` threads the model
   through to `CorpusOptions`.

## Decision

Implementation of ADR-0616 Option A (`--neg` flag) using the following
concrete patterns:

- **`resolution.py`** — adds `MODEL_1080P_NEG`, `MODEL_4K_NEG` constants
  and `neg_model_for(model_version: str) -> str` mapping helper.
  `neg_model_for` is idempotent, passthrough for pre-formatted
  `key=value` model strings, and falls back to appending `"neg"` for
  unknown models so libvmaf surfaces a clear error rather than the
  code silently selecting the wrong model.
- **`cli.py`** — adds `_add_neg_flag(parser)` helper (called for every
  affected subparser), `_resolve_vmaf_model(args)` (replaces every
  `args.vmaf_model` usage in runner functions), and imports
  `neg_model_for` from `resolution`.
- **`ladder.py`** — threads `vmaf_model` parameter through
  `make_default_sampler()` and `_default_sampler()` into
  `CorpusOptions`.
- Affected subcommands: `corpus`, `recommend`, `tune-per-shot`,
  `compare`, `ladder`.

## Alternatives considered

| Option | Decision |
|---|---|
| Route inside argparse `type=` callback | Rejected: would fire at parse time, complicates testing, and prevents `getattr(args, "neg", False)` fallback for subcommands not yet wired |
| Add `vmaf_model` to `bisect_target_vmaf` signature | Not needed: `bisect_target_vmaf` already accepts `vmaf_model` as a parameter; the CLI runner simply passes `_resolve_vmaf_model(args)` |
| Separate `--neg-model` flag for explicit path | Rejected: per ADR-0616, `--model-variant` enum is deferred to V2 |

## Consequences

- **Positive**: `vmaf-tune compare --neg` is correct for codec A vs. B
  comparisons where sharpening could inflate standard VMAF.
- **Positive**: `neg_model_for()` is idempotent and handles edge cases
  cleanly; tests lock down all branches.
- **Negative**: Users must pass `--neg` explicitly; wrong usage (e.g.
  `--neg` for production quality monitoring) produces misleading lower
  scores — mitigated by `docs/metrics/vmaf-neg.md`.
- **Neutral**: `ladder.py`'s `make_default_sampler` grew one keyword
  argument (`vmaf_model`); existing callers are unaffected (default
  preserves historic `vmaf_v0.6.1`).

## References

- Design ancestor: [ADR-0616](0616-vmaf-neg-integration.md)
- User direction: implement ADR-0616 (2026-05-19 session)
- Docs: [docs/metrics/vmaf-neg.md](../metrics/vmaf-neg.md)
- Research: [docs/research/0612-vmaf-neg-integration-research.md](../research/0612-vmaf-neg-integration-research.md)
