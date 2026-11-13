---
paths:
  - scripts/ci/test_git_fixture_isolation.py
  - .pre-commit-config.yaml
invariant: ADR-3535 Python fixture filters exclude whole `GIT_` prefix; fixture files assign global + system config isolation.
area: gates
---
# Fixture Git configuration (ADR-3535)

`git-fixture-isolation-static` hook runs `test_git_fixture_isolation.py`
static methods. `unisolated_git()` + `unisolated_offenders()` enforce Python
rules via `ast.parse()`; unparseable `.py` -> `path:line` violation; keep hook
entry point.

- Python `os.environ` / `os.environ.copy()` / `dict(os.environ)` (+ `.items()`
  / `.keys()`) comprehensions + `filter(...)` calls: fail closed against AST
  allow-list. Comprehension = 1 synchronous generator + 1 `if` clause; `filter`
  = 1-arg `lambda`; condition = only `not name.startswith("GIT_")` or tuple
  beginning `"GIT_"`. Callback predicates, extra generators/`if`s, non-literal
  prefixes -> `path:line` + `ast.get_source_segment` diagnostic.
- Python markers + fixture barriers: marker = `ast.Call` on
  `startswith("GIT_*")` or `clean_git_environment` / `git_environment` (never
  comment/docstring/string). Fixture scope (test-script name, `tests/`,
  basename containing `fixture`) -> executable AST assignment of
  `GIT_CONFIG_GLOBAL` (`os.devnull` or `"/dev/null"`) + `GIT_CONFIG_SYSTEM`
  (`os.devnull` / `"/dev/null"`) or `GIT_CONFIG_NOSYSTEM="1"` via `dict`
  literal, `dict(...)` / `.update(...)` positional mapping or keyword, or
  subscript assignment. `ISOLATION_EXCEPTIONS` excuses missing `GIT_*` marker
  only; fixture barriers still enforced.
- `**os.environ` / copy after `GIT_CONFIG_*` in `dict` literal or
  `dict(...)` / `.update(...)` (positional mapping + keyword order) -> refused.
- Caller-poison `GIT_CONFIG_KEY_*` suffix lookup allowed; no complete-prefix
  exclusion claimed. Product files outside fixture configuration scope.
- Rejected filters/markers/barriers, missing keys, accepted forms, tuple
  prefixes, copied `os.environ` sources, helper-marked + excepted fixtures,
  poison lookup, product scope + planted temporary copies: replayed inside
  `test_no_script_creates_or_configures_a_repository_unisolated` through
  `unisolated_offenders()`.
- Code write scan: `git` at word boundary or right after `_` -> `self._git("init")`,
  `run_git(["clone", url])` flagged; `self._git("status")`, `digit("init")` not.
  Planted cases replayed in same static test.
- Known gaps (`Known gap:` tests): Python separate-statement imperative mutation
  (including after copying `os.environ`), alias/interprocedural data flow, and
  Node/Go filter re-admission. Shell/Node/Go retain regex markers; sentinel run
  stays behavioural coverage.
