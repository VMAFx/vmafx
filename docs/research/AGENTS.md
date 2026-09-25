<!-- markdownlint-disable MD013 -->
# Research Digests — Agent Invariants

Parent: [../../AGENTS.md](../../AGENTS.md). Deep-dive deliverable contract
(research digests = required PR artifacts) set by
[ADR-0108](../adr/0108-deep-dive-deliverables-rule.md).

## Header ID normalization

Filename ID and `# Research-NNNN` header ID must match. Cross-links
(codebase, other docs) use filename ID (e.g., `docs/research/0033-*.md`) ->
header must mirror filename.

**Normalization rule:** new or renamed research digest file:

- extract numeric ID from filename (e.g., `0033` from `0033-foo.md`)
- file starts with `# Research-0033` to match
- no header -> add one as first line (before any subtitle or content)

**Rationale:** filename IDs = stable cross-link targets (git diffs, PR
descriptions, ADR references, commit messages). Headers must track
filenames: prevents audit mismatches, stale references.

`scripts/ci/check-research-digest-ids.py` enforces this for new work and
ratchets the exact inherited collision and heading debt in its generated
baseline. Per ADR-1335, that baseline is valid only when it exactly matches the
current tree and is a debt subset of the trusted merge-base baseline (or the
immutable pre-ratchet tree during first adoption). A baseline entry is not a
reusable exemption. Fix the digest and use ordinary `--write` only against a
trusted canonical baseline; never hand-edit it to admit a new collision or
malformed heading, and never put `--bootstrap-from-ref` in CI or a hook.
