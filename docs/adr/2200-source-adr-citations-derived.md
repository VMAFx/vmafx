<!-- markdownlint-disable MD013 MD060 -->
# ADR-2200: The live source ADR-citation bindings are derived from the tree, not recorded

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: `ci`, `docs`, `process`

## Context

[ADR-1311](1311-source-adr-citation-provenance.md) made the gate
`scripts/ci/check-source-adr-citations.py` bind each plain `ADR-NNNN` citation in
source to an exact decision file and recorded that binding in
`scripts/ci/source-adr-citations.json`: for every live number, the ADR file and
every path that cites it with its count (898 live identities). Any change that
adds, removes or moves one citation changed that file, so two pull requests that
cited ADRs conflicted on it, and a rebase over a moved master re-generated it
with `--write` each time. On GitHub the pull request then showed `CONFLICTING`
and none of its workflows started
([ADR-2197](2197-render-generated-docs-at-landing.md) has the account; the
registry conflicted on every rebase of #2390).

The live binding carries no information a reviewer authored: the file of a number
is the one file `docs/adr/NNNN-*.md`, and the sites are what a scan finds. What
the registry protects is the three refusals of ADR-1311: a citation of a number
that has no ADR file, a retired number reused for another decision, and a
synthetic number cited outside its recorded paths. Those need records only for
the retired identities and the fixtures, which a reviewer writes by hand.

The maintainer decided (Q-082, 2026-10-07) that the checker derives the live
bindings from the tree on every run and the registry keeps only the hand-governed
records.

## Decision

We will derive the live bindings and keep the hand-governed records:

- `check-source-adr-citations.py` computes, on every run, each cited number's ADR
  file and its sites from `git ls-files` and `docs/adr/`. A change that adds or
  moves a citation edits no shared file.
- `scripts/ci/source-adr-citations.json` (schema 2) holds `retired` and `fixtures`
  only. A `live` section is refused, not ignored, so a stale registry cannot pass
  silently. The `--write` mode is removed: nothing is left to write.
- ADR-1311's refusals stay: a citation of a number with no ADR file and no
  retirement record (the message says to file the ADR or audit and record the
  retirement); a retired number that has an ADR file again; a fixture number
  cited outside its exact recorded paths, or with changed counts; and retired
  and fixture counts, which stay exact and hand-authored. The isolation test of
  the checker's Git environment is unchanged.

This supersedes the live-binding part of ADR-1311 (the recorded `live` section
and `--write`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the recorded live section, resolve conflicts at rebase | Reviewers see each new citation in a diff | A conflict on every rebase; GitHub shows the pull request conflicting | The failure this ADR removes |
| Record the live bindings per file (a sidecar next to each source file) | No shared line | A new file per citing source file, drifting with every edit | More state, same information |
| Render the live section at landing like the changelog (ADR-2197) | Keeps the diff of bindings in history | The gate is the reader at pull-request time, so it cannot depend on a render the pull request lacks | Not the same class: the gate reads it |
| Drop the gate | Nothing to maintain | Loses the three refusals | The refusals are the point of ADR-1311 |

## Consequences

- **Positive**: a pull request that cites an ADR touches no registry line, so it
  cannot conflict there; the registry shrinks from 898 live entries plus the
  hand-governed records to the hand-governed records (4 retired, 4 fixture).
- **Negative**: a reviewer no longer sees a new live citation as a registry diff;
  the citation itself is in the source diff, and a number with no ADR file is
  still refused.
- **Neutral / follow-ups**: `scripts/ci/AGENTS.d/adr-citations.md` is updated;
  the research digest of ADR-1311 describes the old registry and stays as the
  record of that decision.

## References

- `Q-082` (maintainer decision, 2026-10-07): derive the live bindings from the
  tree; keep the retired and fixture records; keep ADR-1311's refusals and the
  `GIT_*` isolation test.
- [ADR-1311](1311-source-adr-citation-provenance.md) (superseded in part),
  [ADR-2197](2197-render-generated-docs-at-landing.md).
