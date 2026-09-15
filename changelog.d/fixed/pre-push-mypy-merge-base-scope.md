- Fix pre-push mypy scope after rebases: recheck every branch-owned Python
  file under `ai/` and `scripts/` against the master merge base, including
  unchanged outgoing files and Git type changes. Preserve internal symlink
  filenames, reject unsafe targets and refuse validation of a pushed ref
  different from the checked-out HEAD.
