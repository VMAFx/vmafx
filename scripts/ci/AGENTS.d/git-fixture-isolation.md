---
paths:
  - scripts/ci/test_git_fixture_isolation.py
  - .pre-commit-config.yaml
invariant: ADR-3535 Python fixture filters exclude whole `GIT_` prefix; fixture files assign global + system config isolation.
area: gates
---
# Fixture Git configuration (ADR-3535)

`git-fixture-isolation-static` hook runs `test_git_fixture_isolation.py`
static methods. `unisolated_git()` owns both rules below; keep hook entry
point.

- Python direct `os.environ` comprehensions + `filter` lambdas: complete
  `GIT_` exclusion condition = only `not name.startswith("GIT_")`, or tuple
  beginning `GIT_`. Additional Boolean clauses -> path:line + condition
  diagnostic. Every matching filter checked.
- Python fixture scope: test-script name, `tests/` directory, basename
  containing `fixture`. Git-write + isolation markers -> explicit
  `GIT_CONFIG_GLOBAL=os.devnull`, plus `GIT_CONFIG_SYSTEM=os.devnull` or
  `GIT_CONFIG_NOSYSTEM="1"`. Dictionary, keyword, update, subscript forms
  accepted. Never remove inherited-variable exclusion or configuration keys.
- Caller-poison `GIT_CONFIG_KEY_*` suffix lookup allowed; no complete-prefix
  exclusion claimed. Product files outside fixture configuration scope.
- Rejected filters, missing keys, accepted forms, tuple prefixes, poison
  lookup, product scope + planted temporary copy: replayed inside
  `test_no_script_creates_or_configures_a_repository_unisolated` through
  `unisolated_offenders()`.
- Node/Go re-admission check skipped: callback predicates + imperative
  environment mutation need language-specific extraction. Existing markers
  retained; sentinel run stays behavioural coverage.
