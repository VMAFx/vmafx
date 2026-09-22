- Removed three dead origin-based exemptions from the banned-function
  Semgrep guard (`vmaf-no-strcpy-strcat-sprintf`), which between them
  left four source paths unscanned for `strcpy` / `strcat` / `sprintf`
  while carrying no banned call at all. `.semgrep.yml` excluded
  `/core/tools/cli_parse.c` (deleted under ADR-1155; the parser is
  `cli_parse.cpp`, which the glob never matched), `/core/tools/y4m_input.c`
  (its `strcpy` became a bounded `memcpy` in `bc00da0be`) and
  `/matlab/**` (root-anchored, and no `matlab/` directory exists there —
  the MEX sources are under `compat/python-vmaf/matlab/`);
  `.semgrepignore` hid `compat/python-vmaf/matlab/` plus the
  pre-ADR-0700 `python/vmaf/matlab/`; and the `semgrep-local`
  pre-commit hook skipped `^compat/python-vmaf/matlab/`. All three were
  justified in comments by the files' Netflix origin, which ADR-1142
  retired as a reason. The rule now carries no `paths:` key, and
  `scripts/ci/tests/test_semgrep_vendored_scope.py` plants banned calls
  at the formerly-exempt paths in both scan forms, checks the hook's
  exclude regex, scans the real files, and fails if the rule grows a
  path filter again.
