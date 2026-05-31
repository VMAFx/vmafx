> **Allocator**: run `scripts/adr/next-free.sh --claim <your-topic-slug>` to atomically
> reserve a number before creating this file.  The command creates a
> `docs/adr/NNNN-<slug>.md.stub` placeholder that prevents parallel agents from
> claiming the same number.  Rename the stub to `.md` when you commit the real ADR.
> If you abandon the PR, run `scripts/adr/next-free.sh --release <NNNN>` to free the slot.
> The pre-commit hook (`check-adr-numbering`) and the CI gate (`adr-collision-check` in
> `rule-enforcement.yml`) remain the hard backstop for non-compliant callers.
> See [ADR-0386](0386-adr-numbering-collision-prevention.md) and
> [ADR-0535](0535-adr-atomic-allocator.md).

# ADR-0000: <short, declarative title>

- **Status**: Proposed | Accepted | Deprecated | Superseded by [ADR-NNNN](NNNN-title.md)
- **Date**: YYYY-MM-DD
- **Deciders**: <names / handles>
- **Tags**: <comma-separated area tags — e.g. `ci`, `security`, `cuda`, `simd`, `ai`, `build`, `docs`, `license`, `workspace`, `agents`>

## Context

What problem are we solving? What forces are at play (technical, organisational, regulatory)? Cite constraints from [principles.md §2](../principles.md) (NASA/JPL Power of 10, SEI CERT, SLSA, etc.) where relevant.

Keep it short. One or two paragraphs. The reader should understand *why a decision was needed*, not the full history.

## Decision

State the decision in active voice: "We will use X." One paragraph max.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Option A | … | … | … |
| Option B | … | … | … |

At minimum list the runner-up. If the alternatives section is empty, the decision wasn't real — it was a default.

## Consequences

- **Positive**: what becomes easier, faster, safer.
- **Negative**: what becomes harder, slower, more expensive (operationally or cognitively).
- **Neutral / follow-ups**: things that must happen because of this decision (new tests, docs, migration steps, deprecation timeline).

## References

- Upstream docs, RFCs, blog posts, prior ADRs.
- Related issues / PRs: `#NNN`.
- Source: `req` (direct user quote) or `Q<round>.<q>` (verbatim popup answer from `.workingdir2/decisions/questions-answered.md`).
