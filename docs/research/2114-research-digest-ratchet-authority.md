<!-- markdownlint-disable MD013 MD060 -->
# Research-2114: trusted authority for the research-digest identity ratchet

- **Status**: Complete
- **Date**: 2026-09-25
- **Scope**: research-digest numeric IDs, H1 identity, baseline provenance
- **Decision**: [ADR-1335](../adr/1335-research-digest-identity-ratchet.md)

## Question

Can a generated legacy-debt baseline prevent new research-ID collisions when
the proposed branch is also allowed to create or rewrite that baseline?

## Reproducer and finding

No. The first candidate compared the working tree only with its own JSON. A
branch could add a colliding digest, regenerate canonical JSON, and pass. If the
baseline was deleted, ordinary `--write` recreated it from the modified tree.
Both outputs were byte-deterministic but neither had independent authority.

Comparing the candidate with trusted base `4e6916d16ac57647105d14a47a6680117d6b5738`
made the bypass observable. The trusted tree contained 53 collision sets and
210 H1 exceptions. The self-authored candidate reported 52/211 and had silently
accepted three new defects:

- `2080-codeql-python-alerts-triage-2026-09-24.md`,
  `2080-hip-float-motion-lifecycle-flush.md`, and
  `2080-rust-ci-path-filter-coverage.md` shared one ID;
- Research-1306 and Research-1317 did not begin with their filename-bound
  `# Research-NNNN` H1.

The earliest Research-2080 (required-workflow routing) retains the ID. The HIP
and CodeQL digests move to 2115 and 2116 with their references, while the two H1s
become canonical. The final proposed baseline is 51 collision sets and 209 H1
exceptions: a strict reduction from trusted authority, not a redefinition of it.

## Authority model

The gate uses three comparisons:

1. Current `docs/research/` must exactly equal the branch's canonical JSON.
2. The branch JSON must be a subset of debt at the trusted merge base.
3. Ordinary writes require a canonical baseline at that trusted revision.

For the first adoption only, the trusted revision has no checker or baseline.
The audit derives the same debt model from that immutable Git tree. Creation is
separate: `--bootstrap-from-ref` requires a full 40-character ancestor, refuses
an existing output, verifies that the ancestor predates the ratchet, and rejects
all debt growth. Neither CI nor pre-commit uses bootstrap.

## Adversarial matrix

| Mutation | Branch JSON vs current tree | Branch JSON vs trusted base | Verdict |
|---|---:|---:|---|
| Delete current baseline | unreadable | not evaluated | fail |
| Remove baseline debt without fixing files | mismatch | reduction | fail |
| Add collision and regenerate canonical JSON | match | growth | fail |
| Change one malformed H1 and regenerate | match | changed debt | fail |
| Fix a collision and regenerate | match | reduction | pass |
| Bootstrap from a branch name | not evaluated | mutable ref | fail |
| Bootstrap from exact pre-ratchet ancestor with no growth | match | subset | pass |

The hermetic test fixture creates temporary Git repositories and exercises the
real CLI, including baseline deletion, canonical manual rewrite, trusted-base
growth, missing trusted baseline, mutable bootstrap ref, and post-base debt.

## Operational impact

The check reads one baseline blob and archives only `docs/research/` from the
trusted merge base. It adds no dependency or network access. Full history is
already present in the required Rules workflow. The change affects docs and CI
policy only: no public API, metric score, model, snapshot, Netflix golden
assertion, benchmark, tuning, or retraining surface changes.
