# ADR-3354: A daily watcher files one issue per Netflix commit without a disposition

- **Status**: Accepted
- **Date**: 2026-10-19
- **Deciders**: Lusoris
- **Tags**: ci, github, planning

## Context

The fork records the Netflix/vmaf commit it is at parity with in
`docs/development/known-upstream-bugs.md` (ADR-1474), but nothing reported
upstream commits that landed after that pin. Five Netflix commits waited
unnoticed until a manual sync review found them. The existing watchers
(ADR-0339) only look for named features in FFmpeg.

## Decision

`scripts/upstream-watcher/check_netflix_unported.py` lists the non-merge
commits between the recorded pin and `upstream/master` and drops every commit
that `scripts/upstream-watcher/netflix-dispositions.tsv` records as `ported`,
`in-fork` or `not-applicable`, each row with a reference. The `netflix-unported`
job of `.github/workflows/upstream-watcher.yml` runs it daily and opens one
issue per remaining commit, de-duplicated by its exact title across open and
closed issues. A ledger error, a fetch failure or a failed issue call fails the
job. A port or a disposition adds its ledger row in the same pull request.

## Alternatives considered

| Option | Pros | Cons |
| --- | --- | --- |
| Ledger of dispositions plus daily issue per commit (chosen) | Every upstream commit gets an explicit, reviewed outcome; one issue per commit is easy to close by its port | The ledger must be kept current with each port |
| Match fork commits by subject or `cherry picked from` trailer | No ledger file | Fork ports rewrite subjects and split commits, so matches miss or misfire |
| Move the parity pin forward after each port | Single source | The pin marks full parity; partial ports between pins would stay invisible |
| One summary issue per run | Fewer issues | Hard to close piecemeal; state per commit is lost |

## Consequences

- **Positive**: an upstream commit cannot sit unported without an open issue.
- **Negative**: each port carries one more file edit (its ledger row).
- **Neutral / follow-ups**: commits found on the first run without a verified
  disposition are filed as issues and resolved there.

## References

- [ADR-0339](0339-av1-videotoolbox-placeholder-adapter.md),
  [ADR-1086](1086-ci-workflow-least-privilege-permissions.md) (workflow
  permissions), [ADR-1474](1474-relicense-helper-headers-and-ci-check.md)
- Controller order `vmafx-upstream-sync-20261019` (paraphrased): port the
  five Netflix commits and add a daily watcher that files a de-duplicated
  issue per unported commit.
