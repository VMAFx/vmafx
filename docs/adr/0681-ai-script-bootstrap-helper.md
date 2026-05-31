<!-- markdownlint-disable MD013 MD060 -->
# ADR-0681: AI Script Bootstrap Helper

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: Lusoris, Codex
- **Tags**: ai, cli, provenance, agents

## Context

The AI manifest sweep now has shared run-provenance and CLI helper APIs, but
many directly executable `ai/scripts/*.py` entrypoints still copy the same
startup block: compute `Path(__file__)`, find the repo root, and mutate
`sys.path` before importing `aiutils`, sibling scripts, or `vmaf-tune` helpers.
That boilerplate is unrelated to the artifact schemas and keeps drifting between
scripts.

The helper cannot be imported from `aiutils`, because directly executed scripts
need the bootstrap before `ai/src` is importable. The project rules also require
agent-facing operating instructions to describe the new pattern so future
script work does not reintroduce ad hoc path setup.

## Decision

We will use `ai/scripts/_script_bootstrap.py::bootstrap_ai_script(__file__)` as
the direct-invocation bootstrap for AI scripts. The helper returns resolved
script/repo paths and prepends only explicitly requested repo-local roots:
`ai/src` by default, plus optional repo root, `ai/scripts`, and
`tools/vmaf-tune/src`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep per-script `sys.path.insert(...)` blocks | No new helper and each script is self-contained. | Continues the drift that triggered the AI helper sweep; hard for future agents to know which roots are required. | Rejected because this is boilerplate, not script-specific logic. |
| Put the bootstrap in `aiutils` | Keeps helpers in one package. | Direct scripts cannot import `aiutils` until `ai/src` is already on `sys.path`. | Too late in the import lifecycle. |
| Add a script-local bootstrap helper | Solves direct invocation before `aiutils` imports and keeps options explicit. | Leaves a small private module under `ai/scripts`. | Chosen because it matches the runtime constraint without hiding script behavior. |

## Consequences

- **Positive**: New AI scripts have one documented path bootstrap instead of
  open-coded `sys.path` mutations.
- **Positive**: Manifest/materializer scripts can share startup mechanics while
  keeping their table schemas and report formats local.
- **Negative**: Scripts that need a new repo-local import root must extend the
  helper and its tests instead of patching one script.
- **Neutral / follow-ups**: Remaining historical AI scripts can migrate to the
  helper when their owning workstream is touched.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md)
- [ADR-0680](0680-ai-cli-helper-pattern.md)
- [Research-0701](../research/0701-ai-script-bootstrap-helper.md)
- Source: `req` — "yeah i guess you can do this on a lot of script in ai... and you need to keep claude skills and agent.mds etc in line with the boilerplate/templates you create"
- Source: `req` — "dont just wait for ci, do more backlogs"
