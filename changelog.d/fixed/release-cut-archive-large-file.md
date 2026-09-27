- A release cut whose changelog archive is larger than 1 MB can now be
  committed. Top-level Markdown files in `docs/changelog-archive/` are exempt
  from the `check-added-large-files` pre-commit gate
  ([ADR-1345](../docs/adr/1345-changelog-archive-large-file-exemption.md)).
  The 1.0.0-rc.1 archive holds the fork's whole fragment history (1.86 MB), and
  the gate refused its cut commit.
