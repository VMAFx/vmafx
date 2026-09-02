# Research 0129: ensemble training kit doc status

## Scope

The human-facing ensemble-training-kit page still described the kit as a
proposed packaging surface even though the tool bundle exists in tree and
ADR-0324 has been accepted.

## Findings

- `tools/ensemble-training-kit/` exists with the five numbered step
  scripts, `run-full-pipeline.sh`, `make-distribution-tarball.sh`,
  `extract-corpus.sh`, `prepare-gdrive-bundle.sh`, frozen Python
  requirements, a README, an AGENTS file, and a shell smoke test.
- `docs/adr/0324-ensemble-training-kit.md` is already marked
  `Accepted` and carries a 2026-05-08 acceptance update.
- `docs/adr/_index_fragments/0324-ensemble-training-kit.md` still
  listed ADR-0324 as `Proposed`, which made the generated ADR index
  disagree with the ADR body.
- `.workingdir2/BACKLOG.md` already marks `TA-KIT` as `DONE` with
  PR #429 and the Google Drive quickstart follow-up.

## Decision

Refresh the operator page and ADR index fragment only. No code changes
are needed: the release-facing package already exists, and this PR
aligns the docs with the shipped state.

## References

- User request: "and another one"
- `tools/ensemble-training-kit/README.md`
- `docs/adr/0324-ensemble-training-kit.md`
