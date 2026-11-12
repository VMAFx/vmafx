- **New Bandit findings in changed Python files now fail at pre-commit and pre-push, before Codacy sees them.**
  The `bandit-new` hook (`scripts/ci/check_bandit_new.py`) runs Bandit with the 56 Codacy-enabled
  patterns in `bandit.yml` and reports only findings the merge-base copy of the file does not carry,
  so moved lines and Git-detected renames keep their old findings. Pre-push scans the outgoing
  commit rather than local edits. Separate `bandit-new-push` and `flawfinder-new-push` hooks use
  `--outgoing` to scan the full pre-push merge-base delta without framework filenames, including
  docs-only follow-up pushes and files absent from the worktree. Configuration changes scan all
  tracked Python files with each revision's pattern list only in the invocation that selects
  `bandit.yml`, avoiding repeated full scans across partitioned file lists; Python stubs, Python
  executables and PEP 263 encodings are supported. The default comparison follows `origin/HEAD`,
  falling back to `origin/master`. A `# noqa` comment does not silence it. The flawfinder gate
  shares the same merge-base comparison (`scripts/lib/new_scanner_findings.py`).
