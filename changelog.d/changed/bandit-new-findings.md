- **New Bandit findings in changed Python files now fail at pre-commit and pre-push, before Codacy sees them.**
  The `bandit-new` hook (`scripts/ci/check_bandit_new.py`) runs Bandit with the 56 Codacy-enabled
  patterns in `bandit.yml` and reports only findings the merge-base copy of the file does not carry,
  so moved lines and renamed files keep their old findings. A `# noqa` comment does not silence it.
  The flawfinder gate shares the same merge-base comparison (`scripts/lib/new_scanner_findings.py`).
