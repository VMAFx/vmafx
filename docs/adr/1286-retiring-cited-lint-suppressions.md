<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1286: A cited lint suppression is retired only by a refactor that satisfies the invariant it cites

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: code-quality, process, agents, ci

## Context

[ADR-0141](0141-touched-file-cleanup-rule.md) §2 makes `// NOLINT` legal only
where a refactor "would break a load-bearing invariant", and requires every
suppression to cite that invariant inline;
[ADR-0278](0278-t7-5-nolint-sweep.md) finished paying off the untagged
historical debt, so every `NOLINT` now in tree carries a citation.
[ADR-1142](1142-whole-codebase-standards.md) then extended the standards to the
whole tree, which put the sweeps that clear those standards on a collision
course with the suppressions: a sweep touches a file, ADR-0141 says leave it
clean, and the file already says "this one cannot be cleaned, here is why".

Both rules point at the same question and neither answers it: *when may a
sweep delete a citation that a previous PR deliberately wrote?* The
HISS-21 `core/test/` pass ran straight into it. Five of the six
`readability-function-size` citations it met were removable — their text
named a consequence ("splitting hides which assertion fired", "obscures the
`sycl_state` ownership model") that the replacement refactor demonstrably
avoids, or named no invariant at all ("test scaffolding"). The sixth, in
`core/test/test_barten_csf.c`, named an invariant the fork cannot satisfy by
refactoring: the body is Netflix upstream's assertion sequence carried
verbatim, and upstream keeps appending cases to it, so any reshaping
converts every future sync of that file from a diff-and-merge into a
hand-merge. An earlier revision of the pass removed all six. The review that
followed was right to stop it, and the difference between the five and the one
is not visible from the citation's presence alone.

## Decision

We will treat a cited suppression as retirable **only when the change that
removes it satisfies, in the tree, the specific invariant the citation
names** — and we will judge that against the invariant, not against the
citation's prose. Three consequences follow, and all three are the author's
burden, not the reviewer's:

1. A citation naming a *consequence internal to the fork* ("splitting hides
   which assertion fired") is retirable by a refactor that avoids the
   consequence; the change must say in the PR body or the accompanying
   research digest *how* it avoids it.
2. A citation naming an *external contract the fork does not control* —
   upstream-verbatim text whose sync story depends on its shape, an
   ADR-0138 / ADR-0139 bit-exactness pattern, an ABI-visible identifier — is
   not retirable by refactoring at all. Retiring it requires an ADR that
   argues the contract itself is gone.
3. A citation naming *no invariant*, only a category ("test scaffolding"),
   states no claim to satisfy. It is retirable by any refactor that removes
   the diagnostic, and must not be reworded into a stronger claim to keep it.

A comment left behind a retired suppression states what the code does. A
sweep may not replace "held verbatim, splitting would break the sync" with
"held verbatim" over a diff that reshapes the text.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Retire a citation only when the refactor satisfies the named invariant, with the three-way split above (chosen)** | Keeps ADR-0141's refactor-first default intact where it works; the one class of suppression that protects something the fork does not own stays protected; the test is stated against the tree, so a reviewer can check it | Author must read and answer each citation rather than deleting it with the code it sat on | **Decision** — the HISS-21 pass showed the undifferentiated rule deletes the load-bearing case along with the boilerplate |
| Treat every cited suppression as frozen until a superseding ADR | Zero risk of losing a real invariant | Freezes ~20 suppressions whose text names no invariant; every sweep that touches one needs an ADR to delete a stale comment; contradicts ADR-0141's own "prefer a real refactor" precedence | Rejected — pays an ADR per category label |
| Treat every cited suppression as retirable by any refactor that clears the diagnostic | Simplest; sweeps never stall | Exactly what produced the finding: the upstream-parity case cleared the diagnostic and broke the sync story, and no gate can see that | Rejected — the diagnostic count is not the invariant |
| Let the clang-tidy ratchet arbitrate (a removal that regresses fails; one that does not is fine) | Mechanical, already a required gate | The ratchet measures diagnostics, not contracts. `test_barten_csf.c` measured 0 both before and after the split that broke its sync story | Rejected — the gate is blind to this class by construction |

## Consequences

- **Positive**:
  - The suppressions that protect something the fork does not own
    (upstream-verbatim text, bit-exactness patterns) survive the whole-tree
    sweeps ADR-1142 set in motion.
  - A citation becomes a claim a reviewer can test, which pushes new
    suppressions towards naming a real invariant instead of a category.
  - Reviewers get a stated test for a class of finding that previously
    depended on someone remembering the original PR.
- **Negative**:
  - A sweep author pays reading cost per citation encountered, and has to
    write down the argument for each retirement.
  - Class 2 leaves permanently un-clearable findings in the scanner's count.
    That is a real cost and is the price of the sync story; the
    *Deliberate residue* section of a burn-down digest is where they are
    recorded.
- **Neutral / follow-ups**:
  - `core/test/AGENTS.md` records the `core/test/` instance so the next
    agent does not re-split `test_barten_csf.c`;
    [`docs/rebase-notes.md`](../rebase-notes.md) records the conflict
    instruction.
  - No new gate. The clang-tidy ratchet cannot see this class (it measured
    `test_barten_csf.c` at 0 before and after the split), so this is a
    review rule, and the PR template's deep-dive checklist is where it is
    exercised.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) §2 — NOLINT reserved for
  load-bearing invariants, every one cited inline.
- [ADR-0278](0278-t7-5-nolint-sweep.md) — citation closeout; every
  in-tree NOLINT carries an inline citation.
- [ADR-1142](1142-whole-codebase-standards.md) — whole-tree standards and the
  clang-tidy ratchet that enforces them.
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — the bit-exactness class of
  invariant named in Decision §2.
- `docs/research/core-test-hiss21-burndown-2026-09-21.md` — the HISS-21
  `core/test/` pass, its five retirements and its one *Deliberate residue*.
- Source: per reviewer direction 2026-09-21 (paraphrased: an ADR-cited
  load-bearing invariant was reversed without an ADR; either restore the
  suppression verbatim or write the superseding ADR, and do not leave a
  comment that says something the code does not do).
