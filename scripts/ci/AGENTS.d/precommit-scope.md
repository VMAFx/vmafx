---
paths:
  - scripts/ci/precommit_scope.py
  - scripts/ci/tests/test_precommit_scope.py
  - .github/workflows/lint-and-format.yml
invariant: PR Pre-Commit = diff only if no hook input changed; input change or unreadable diff -> whole tree.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Pre-Commit scope (`precommit_scope.py`, HISS-18)

PR -> `pre-commit run --from-ref <base> --to-ref HEAD`; push, tag, schedule,
dispatch -> `--all-files`. Defect seen: #3237 added hook plus script,
diff-only run passed, master push (`a3ea48309`) red on whole tree.

- Hook inputs (`hook_inputs()`): `.pre-commit-config.yaml`; every file under
  `scripts/ci/` (CI authority, see `_index.md`) and `scripts/lib/`; every path
  named by hook command: `entry` plus `args`, local and remote repos
  (remote `entry` override), commands inside `bash -c` / `sh -c`, dotted
  `python -m` modules, `Makefile` for `make`; Python inputs followed through
  imports.
- Missing file still counts: PR deletes or renames entry script.
- Bare executable names (`python`, `bash`) never match directory.
- Fail closed: unreadable file list (git exit, timeout, output flood), bad
  base sha, entry `shlex` fails to split -> whole tree.
- Reason printed to stderr every run; narrowed run never reported as full.
- Test: `tests/test_precommit_scope.py` (21; 7 fail against diff-only
  selector of `634e636a7`).
