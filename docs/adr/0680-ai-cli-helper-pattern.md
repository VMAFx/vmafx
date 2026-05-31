<!-- markdownlint-disable MD013 MD060 -->
# ADR-0680: Shared AI CLI Helper Pattern

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: Lusoris, Codex
- **Tags**: ai, cli, provenance, agents

## Context

The AI manifest sweep removed a lot of repeated run-provenance JSON, but the
new batch materializer scripts still repeated the same operator-facing wrapper
flags: manifest path, base directory, report outputs, raw argv capture, and
fail-fast policy. That boilerplate is not table-specific, and letting it drift
would make future batch runners harder to document and harder to audit.

The project also treats `AGENTS.md` and `.claude/skills/` as executable
operating rules. A helper that is not recorded in those rules will be easy for
future agents to miss during the next AI or tune sweep.

## Decision

We will use `aiutils.cli_helpers` for shared AI CLI boilerplate. New
operator-facing scripts should use `make_argument_parser()` and
`collect_cli_argv()` when they fit the standard pattern. Batch manifest runners
must use `add_batch_manifest_arguments()` for the common manifest/report/fail
flags while keeping table-specific parser fields local.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep parser code local to every script | Maximum local clarity and no helper API. | Repeats the same flags and raw-argv capture in every batch runner. | This keeps the maintenance issue that triggered the dedup request. |
| Build one generic batch-runner framework | Could centralize parsing, loading, reporting, and execution. | Would hide materializer-specific validation and make the current stack harder to review. | Too much abstraction before more packages prove the same shape. |
| Add a narrow CLI helper | Removes repeated parser boilerplate while preserving per-script domain logic. | Scripts still need local manifest schema and table validation. | Chosen because it improves consistency without rewriting the materializers. |

## Consequences

- **Positive**: Batch runners share the same manifest/report/fail-fast operator
  flags and provenance argv capture.
- **Positive**: Future AI scripts have a documented helper before another
  ad hoc parser shape is added.
- **Negative**: The helper intentionally does not cover manifest schema
  validation, so script authors still need local tests for their table fields.
- **Neutral / follow-ups**: Apply the same pattern to larger non-AI tool
  families only after a package-local helper is justified by real duplication.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md)
- [ADR-0678](0678-ai-run-manifest-helper.md)
- Source: `req` — "somehow all you are doing now feels like it should be deduplicated? or are those adapter style scripts?"
- Source: `req` — "yeah i guess you can do this on a lot of script in ai... and you need to keep claude skills and agent.mds etc in line with the boilerplate/templates you create"
- Source: `req` — "we can even do that when done per tool -> cross toll afterwards... its all py scripts i guess"
