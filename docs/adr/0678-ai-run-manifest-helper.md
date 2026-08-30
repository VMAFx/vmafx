<!-- markdownlint-disable MD013 MD060 -->
# ADR-0678: Shared AI Run Manifest Helper

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: Lusoris, Codex
- **Tags**: ai, provenance, docs, agents

## Context

ADR-0661 introduced shared `run_provenance` blocks for AI artifacts, but many
scripts still built their own JSON envelope around that block. The repeated
shape is easy to drift: one script records counts under top-level keys, another
uses `config`, another forgets a missing optional path, and new agent sessions
have to rediscover the intended boilerplate.

The project also treats Claude skills and `AGENTS.md` notes as operational
rules. If the helper pattern is not in those rules, future scripts can regress
back to hand-rolled manifests even when tests pass.

## Decision

We will use `aiutils.run_manifest.write_run_manifest()` for new standalone AI
artifact sidecars. The helper owns the common envelope (`schema` plus
`run_provenance`) and accepts script-specific adapter sections for row counts,
selected features, gates, or configuration. Scripts that already have a stable
report schema may continue embedding only `build_run_provenance()`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep only `build_run_provenance()` | Minimal API, no migration. | Every script still repeats the envelope and can drift. | It does not solve the duplication called out during the AI manifest sweep. |
| Force every report through one universal schema | Maximum consistency. | Breaks existing report consumers and hides script-specific evidence behind a generic shape. | Existing reports are user-facing and should not be renamed just to reduce helper code. |
| Add `write_run_manifest()` for standalone sidecars | Deduplicates the repeated envelope while preserving adapter-specific fields. | Scripts still choose their own `sections` keys. | Chosen because it removes boilerplate without breaking report schemas. |

## Consequences

- **Positive**: New dataset, feature-table, fetcher, quantization, and model
  exporter sidecars share deterministic path evidence and CLI provenance.
- **Positive**: `.claude/skills/ai-run-manifest/SKILL.md` gives agents a
  concrete workflow before adding another script-local manifest writer.
- **Negative**: Existing scripts that embed `build_run_provenance()` into stable
  reports remain mixed until touched for real changes.
- **Neutral / follow-ups**: Continue converting standalone sidecar writers when
  their scripts are touched; do not churn stable report schemas only for style.
- **Neutral / follow-ups**: Audit non-AI tools for the same duplication pattern
  once the AI helper has settled. Do package-local helper passes first
  (`vmaf-tune`, MCP/dev-probes), then consolidate cross-tool pieces once the
  repeated shape is proven.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md)
- [ADR-0668](0668-ai-derived-table-provenance.md)
- [ADR-0677](0677-ai-dataset-fetch-manifests.md)
- Source: `req` — "somehow all you are doing now feels like it should be deduplicated? or are those adapter style scripts?"
- Source: `req` — "yeah i guess you for sure need to add some of those claude skills etc..."
- Source: `req` — "we should widen that later and do that with all tools lol... less work, less code, less maintenance and fixing..."
