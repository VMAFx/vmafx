<!-- markdownlint-disable MD013 MD060 -->
# ADR-0026: Relocate Python harness workspace under python/vmaf/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, python, docs

## Context

Upstream Netflix/vmaf puts the classic Python training/eval harness scratch tree (`dataset/`, `model/`, `encode/`, `output/`, `workdir/`, `result_store_dir/`, `checkpoints_dir/`, `model_param/`) at `ROOT/workspace/`. That clutters the repo root with content that only the Python harness consumes. User: "move/integrate that better in our new rules/structure/philosophy".

## Decision

We will move the harness workspace to `python/vmaf/workspace/` (next to the only code that uses it) and resolve paths via a `WORKSPACE` constant in `config.py`, overridable with the `VMAF_WORKSPACE` env var. Documented in `docs/architecture/workspace.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave at repo root + document | Zero code churn | Root stays noisy despite other cleanup (ADRs 29–34) | Too noisy |
| Move to `ai/workspace/` | One `ai/` tree | Conflates classic SVM harness with fork tiny-AI (different frameworks, different ownership) | Rejected |
| Move to `python/vmaf/workspace/` (chosen) | Co-locates scratch with its only consumer; env override retained | Requires updating every path reference | Rationale: matches the repo-root cleanup family |
| Delete and auto-create | Minimal tree | Loses `.gitignore` subdir contract; lowers discoverability | Rejected |

Rationale note: option (c) chosen; co-locates scratch with the only code that uses it and keeps tiny-AI strictly under `ai/`. The `VMAF_WORKSPACE` env override preserves pointing at a big-disk mount.

## Consequences

- **Positive**: repo root reserved for external-facing surfaces; scratch lives with its consumer.
- **Negative**: every doc/reference to `workspace/` had to update.
- **Neutral / follow-ups**: documented in `docs/architecture/workspace.md`.

## References

- Source: `req` (user: "move/integrate that better in our new rules/structure/philosophy")
- Related ADRs: ADR-0029, ADR-0030, ADR-0031, ADR-0032, ADR-0033, ADR-0034
