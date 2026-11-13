- **PR Title Gate: every pull-request title is held to the Conventional-Commit
  rules in CI (#2750).** The title becomes the subject of the squashed commit
  on `master` and release-please builds the changelog from it, but the
  commit-msg hook only ran on machines with the hooks installed. A new
  `PR Title Gate` required check (`scripts/ci/check-pr-title.py`) validates the
  title against the one shared type list in
  `scripts/lib/conventional_commits.py` — the same list the hook enforces and
  the PR-type label workflow labels by, pinned together by
  `scripts/ci/tests/test_conventional_commits_shared.py`; the label workflow
  drops its own copy of the list (and the `security`/`style` entries the hook
  has always refused), and `docs/principles.md` names the CI check next to the
  hook. The gate ships the negative cases the issue names: an unknown type, a
  missing subject, a breaking change without `!` or footer.
