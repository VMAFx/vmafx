<!-- markdownlint-disable MD013 MD060 -->
# ADR-0014: VSCode uses clangd, disable MS C/C++ IntelliSense

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: build, framework, lint

## Context

Two competing C/C++ language servers are common in VSCode: Microsoft's C/C++ IntelliSense and `clangd`. Having both active produces conflicting diagnostics and jump-to-definition drift. This fork relies on `clang-tidy` and `compile_commands.json` which clangd consumes natively.

## Decision

We will use `clangd` as the C/C++ LSP in VSCode and explicitly disable MS C/C++ IntelliSense via `.vscode/settings.json`, suppressing the MS extension via `extensions.json`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| MS C/C++ IntelliSense | Familiar defaults | Doesn't integrate with `clang-tidy` / `compile_commands.json` our way | clangd matches our lint chain |
| Both active | "Best of both" | Double-diagnostics noise; confusing errors | Always wrong |
| clangd only (chosen) | Single source of truth | Requires explicit opt-out of MS extension | Correct for this stack |

This decision was a default — the alternative of having both active was never viable.

## Consequences

- **Positive**: diagnostics match `make lint`; one definition of "correct".
- **Negative**: contributors who prefer MS extension must override locally.
- **Neutral / follow-ups**: `compile_commands.json` sync hook keeps clangd fresh.

## References

- Source: `Q4.2`
- Related ADRs: ADR-0018
