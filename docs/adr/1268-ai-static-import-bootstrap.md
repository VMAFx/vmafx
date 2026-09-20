# ADR-1268: AI direct-script bootstrap must preserve static imports

- **Status**: Accepted
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: ai, python, lint, imports, cli

## Context

[ADR-0681](0681-ai-script-bootstrap-helper.md) centralised the path setup needed
by directly invoked `ai/scripts/*.py` entrypoints. Each script imported the
helper, called it at module scope, and only then imported repository packages.
That ordering made direct invocation reliable, but every repository import was
necessarily an `E402` violation. The violations were hidden with `# noqa: E402`
comments, leaving 253 findings when the whole-tree audit deliberately ignored
suppression comments.

The warning is not cosmetic: imports split by executable module-level code are
harder for static tooling to analyse and make it easy for future startup work to
depend on import order accidentally. Removing direct invocation is not an
acceptable cleanup because the scripts and their documentation expose
`python ai/scripts/<name>.py` as the supported operator workflow.

## Decision

Importing `ai/scripts/_script_bootstrap.py` will install the complete,
repository-owned import-root set before that import returns: repository root,
`ai/src`, `ai/scripts`, and `tools/vmaf-tune/src`. Direct scripts keep a normal
static import block. Their call to `bootstrap_ai_script(__file__, ...)` moves
after the import block and remains only to produce script-specific path metadata
used by provenance manifests.

The helper resolves every injected path from its own checked-in location; it
does not read `PYTHONPATH`, the current directory, or user-controlled input.
The existing idempotent prepend operation prevents duplicates. Tests pin both
the import-time root installation and the script-specific metadata contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `# noqa: E402` on repository imports | No runtime change | Hides a whole-tree warning category and preserves split import blocks | Rejected by ADR-1267's zero-debt rule |
| Replace static imports with `importlib` calls | Avoids `E402` mechanically | Degrades static analysis and merely evades the rule | Rejected as analyzer evasion |
| Require every operator to install the AI wheel or set `PYTHONPATH` first | Conventional package imports | Breaks documented direct invocation and existing automation | Rejected as a user-visible regression |
| Bootstrap all fixed repository roots when the helper is imported | Preserves direct invocation and ordinary static imports | Adds a deliberate, bounded import side effect in one private helper | Chosen |

## Consequences

- **Positive**: AI scripts retain direct execution with statically analysable,
  suppression-free import blocks.
- **Positive**: one tested helper owns the complete set of repository import
  roots, so scripts cannot drift into private path recipes.
- **Negative**: importing the private bootstrap helper prepends four
  repository-owned paths even when a particular script uses only one of them.
- **Neutral / follow-ups**: script-specific path metadata and manifest
  entrypoint identity remain unchanged; this ADR refines and supersedes only
  ADR-0681's call-before-import ordering.

## References

- [ADR-0681](0681-ai-script-bootstrap-helper.md)
- [ADR-1267](1267-whole-tree-zero-debt-completion.md)
- [Research-2068](../research/2068-whole-tree-zero-debt-inventory.md)
- Source (`req`, verbatim, 2026-09-20): "there is no on touch rule anymore, no
  fucking warning or error is just ignored because of being og netflix code,
  fix them all ffs, how often do i have to repeat thi".
