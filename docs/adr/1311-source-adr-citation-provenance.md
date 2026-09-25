<!-- markdownlint-disable MD013 MD060 -->

# ADR-1311: Bind source ADR citations to exact decisions and governed retirements

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `ci`, `documentation`, `provenance`, `adr`, `source-code`

## Context

Plain `ADR-NNNN` references in implementation and build files carry no slug.
That makes their identity depend entirely on a four-digit number. Two ADR
collision sweeps moved decisions while leaving citations behind, so some old
numbers later resolved to unrelated decisions and looked authoritative. Other
numbers never acquired an ADR file because the proposed work was abandoned,
and genuinely superseded decisions still need to be named in comments that
explain what replaced them.

The Markdown-link checker cannot protect these references: source comments do
not have a link target or slug to compare. Requiring every prose occurrence in
the repository would create false positives in human documentation and test
fixtures. Checking only whether a number currently exists is also insufficient:
number reuse turns a dangling citation into a misleading, apparently valid one.

## Decision

Add an always-run source-citation gate with a committed registry:

1. Scan tracked implementation and build/control files selected by an explicit
   extension and basename allowlist. Markdown, changelog prose, binary assets,
   patches, and other ordinary prose are outside this gate.
2. Record every live citation as an exact ADR filename plus an exact
   `path -> occurrence count` site map. A new, removed, or renumbered source
   citation changes the registry and therefore requires review. Binding the
   filename as well as the number makes later number reuse fail closed.
3. Keep missing historical identities in a separate retirement map. Every
   retirement states its status and reason, points to durable repository and
   Git-history evidence, and names an accepted successor or related ADR when
   one exists. A retired number must remain absent from `docs/adr/`; silently
   reallocating it is an error.
4. Admit synthetic numbers only through exact fixture-number/path entries.
   They are test data, not project decisions, and an occurrence anywhere else
   is an error. Disposable Git fixtures strip inherited `GIT_*` repository
   variables and caller Git configuration, then disable hooks and signing, so
   a commit hook cannot redirect fixture writes into the caller's index.
5. `--write` may regenerate only the mechanically derived live bindings. It
   never invents a retirement or fixture exemption, and it refuses an unknown
   missing number. Changes to the registry remain normal reviewed source diffs.

The registry validates identity and provenance, not whether prose correctly
summarises a decision. Review still owns semantic accuracy; the gate ensures a
citation cannot silently become dangling or acquire a different decision
identity.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Exact live bindings plus governed retirements and fixtures (chosen)** | Detects dangling references, number reuse, source-site drift, and unreviewed fixture escape; preserves truthful historical names | Registry changes when citation sites change; semantic review is still required | It is the smallest fail-closed mechanism that distinguishes current, retired, and synthetic identities without rewriting history. |
| Check only that `docs/adr/NNNN-*.md` exists | Small checker and no registry | Number reuse resolves to an unrelated ADR and passes | This is the defect class that made a valid-looking citation worse than a missing one. |
| Replace every source citation with a Markdown link or inline slug | Self-describing at each site | Thousands of noisy source edits; many languages and linters do not accept one common syntax | Excessive churn and rebase cost for an internal provenance repair. |
| Heuristically compare comment words with ADR titles | No committed registry | Domain vocabulary overlaps heavily; short comments and generated names create false positives and false negatives | A probabilistic gate cannot be fail closed without blocking ordinary prose. |
| Delete or repoint every missing historical number | Removes dangling tokens | Erases supersession history or falsely attributes abandoned work to a later ADR | Historical provenance is data, not cleanup noise. |

## Consequences

- **Positive**:
  - Source citations are bound to the exact decision filename reviewers audited.
  - Retired and abandoned identities remain truthful and cannot be silently
    reassigned.
  - Synthetic test numbers stay narrowly scoped to their fixture paths.
- **Negative**:
  - Adding or removing a citation requires a small registry update.
  - The initial registry is sizeable because it records the existing source
    corpus rather than grandfathering it invisibly.
- **Neutral / follow-ups**:
  - Human review remains responsible for whether the cited decision actually
    supports the surrounding sentence.
  - Markdown links remain governed by `scripts/ci/check-adr-links.py`; this gate
    deliberately owns a different surface.

## References

- [ADR-0278: NOLINT citation closeout](0278-t7-5-nolint-sweep.md)
- [ADR-0386: ADR collision prevention](0386-adr-numbering-collision-prevention.md)
- [ADR-0535: Atomic ADR allocator](0535-adr-atomic-allocator.md)
- [ADR-1142: Whole-codebase standards](1142-whole-codebase-standards.md)
- [Research-1311: source ADR citation provenance audit](../research/1311-source-adr-citation-provenance-audit.md)
- Source: `req` — "fully resolve open docs/state row `T-STALE-ADR-CITATIONS-2026-09-16`" and add a fail-closed regression gate without ordinary-prose false positives.
