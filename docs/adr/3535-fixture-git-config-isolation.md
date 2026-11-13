# ADR-3535: Isolate fixture repositories from inherited Git configuration

- **Status**: Accepted
- **Date**: 2026-11-11
- **Deciders**: Maintainers
- **Tags**: ci, testing, git

## Context

At snapshot `6f42cb9fa`, the measurement recorded on 2026-11-11 at 17:32 CET
counted 46 Python files matching the Git-write and isolation markers. Sixteen
lacked explicit global and system configuration isolation: fifteen fixture files
and one production file, `tools/rc1-tester/image/licensing.py`. Temporary
repositories could consequently inherit `commit.gpgsign` from caller
configuration.

`scripts/ci/test_git_fixture_isolation.py` accepted an isolation marker anywhere
in a file. A comprehension could exclude `GIT_*` and then re-admit caller
`GIT_CONFIG_*` variables through an additional Boolean clause. Another correct
comprehension elsewhere in the file could also hide the unsafe one. The shell
helper, `scripts/lib/clean-git-env.sh`, already ignores global and system
configuration.

## Decision

Extend `unisolated_git()` and `unisolated_offenders()` through the existing
`test_no_script_creates_or_configures_a_repository_unisolated` method and
`git-fixture-isolation-static` hook. Keep the Python checks and their replay
cases on that existing entry point.

Parse Python files with `ast.parse()` in
`scripts/ci/test_git_fixture_isolation.py` and fail closed with the file path
and `SyntaxError` line when source cannot be parsed. Decide Python isolation
markers from executable `ast.Call` nodes (`startswith` with a `GIT_`-prefixed
string constant or tuple containing one, or a call to `clean_git_environment` /
`git_environment`), so comments, docstrings and string literals never count as
markers. Validate every comprehension and `filter(...)` call whose iterable
derives from `os.environ` (`os.environ`, `os.environ.copy()`,
`dict(os.environ)`, and `.items()` / `.keys()` on each) against an explicit AST
allow-list. A comprehension is valid only with one synchronous generator and one
`if` clause consisting solely of `not <key>.startswith("GIT_")` (or a tuple of
string constants starting with `"GIT_"`), or the single planted
`GIT_CONFIG_KEY_*` suffix lookup; `filter(...)` is valid only with a
single-argument `lambda` carrying that canonical exclusion. Callback predicates,
extra generators or `if` clauses, missing `if` clauses and non-literal prefixes
fail with `<file>:<line>` and `ast.get_source_segment()`.

For Python fixtures that create or configure repositories, require executable
AST assignments setting `GIT_CONFIG_GLOBAL` to `os.devnull` or `"/dev/null"` and
either `GIT_CONFIG_SYSTEM` to `os.devnull` / `"/dev/null"` or
`GIT_CONFIG_NOSYSTEM` to `"1"`. Accept dictionary literals, positional mapping
or keyword arguments on `dict(...)` or `<mapping>.update(...)`, and subscript
assignments, refusing `**os.environ` (or a copy) unpacked after a barrier key in
the same mapping display or call (including barrier keys inside a positional
dictionary display or nested mapping call); ignore comments, docstrings, string
literals and `setdefault`.
Fixture scope includes existing test-script names, files under a `tests/`
directory and Python basenames containing `fixture`. Run the barrier check on
every isolated Python fixture (both `startswith`-marked and helper-marked) as
well as Python fixtures listed in `ISOLATION_EXCEPTIONS`, where exceptions
excuse only the missing `GIT_*` drop marker.

Apply the missing barriers and canonical `os.environ` filters across the
affected fixture files and fixture helpers while preserving additional dropped
variables. Keep production licensing behavior unchanged: its temporary
archive-fetch repositories remain outside the fixture configuration rule.

Shell, Node and Go retain the regex isolation-marker path. Python callback
predicates on `filter(...)` and post-barrier `**os.environ` (or copied
`os.environ`) expansions in dictionary displays and `dict(...)` / `.update(...)`
calls (across positional mapping arguments and keyword arguments) are refused by
the AST check; separate-statement imperative environment mutation (including
after copying `os.environ`), alias/interprocedural data flow and Node/Go filter
re-admission remain outside the static check and are pinned with `Known gap:`
replay tests in `scripts/ci/test_git_fixture_isolation.py`. The agent invariant
page `scripts/ci/AGENTS.d/git-fixture-isolation.md` records these rules and
limits.

## Alternatives considered

| Option | Advantages | Limitations | Outcome |
| --- | --- | --- | --- |
| AST parse | Exact expression structure, executable call/assignment forms and source locations; ignores comments, docstrings and string literals | Requires parseable complete Python; does not track imperative mutation, aliases or interprocedural data flow | Chosen for Python |
| Text allow-list with token boundaries | Small explicit pattern set without language-specific AST machinery | Raw-source regexes match strings and comments; bounded token headers miss multi-generator comprehensions, copied `os.environ` mappings and non-literal predicates | Rejected for Python; regex marker path kept for shell, Node and Go |
| Runtime-only sentinel | Detects observable writes to a temporary caller repository | Cannot cover every product script or pytest-only fixture; configuration inheritance need not damage the sentinel | Retained as complementary coverage |

## Consequences

- Fixture commits ignore caller signing defaults and all inherited `GIT_*`
  variables, including excepted Python fixtures.
- Noncanonical `os.environ` comprehensions and `filter(...)` calls fail closed
  even without an inline `GIT_` literal or when iterating over
  `os.environ.copy()` / `dict(os.environ)`; separate `.pop()` operations
  preserve additional removals.
- The existing static test replays rejected filters, non-executable markers and
  barriers, post-barrier `**os.environ` expansions, missing keys, accepted
  assignment styles, tuple prefixes, copied `os.environ` sources, helper-marked
  and excepted fixtures, caller-poison selection and a product-file exemption.
  Temporary copies of `test_ci_impact.py` and excepted `test_merge_train_guard.py`
  fail `unisolated_offenders()` when a bypass or missing barrier is planted and
  pass after restoration.
- Configuration assignments, barrier-before-unpack ordering in mapping displays
  and `dict(...)` / `.update(...)` calls (including positional mapping
  arguments), and markers are checked via AST within each Python file.
  Separate-statement imperative mutation (including after copying `os.environ`),
  alias handling, interprocedural data flow, assignment-to-subprocess
  association and Node/Go filter re-admission remain outside this check and are
  replayed by `Known gap:` tests.

## References

- [Static check and replay
  cases](../../scripts/ci/test_git_fixture_isolation.py)
- [Shell environment helper](../../scripts/lib/clean-git-env.sh)
- [Hook documentation](../development/pre-commit-hooks.md)
- [Agent invariant](../../scripts/ci/AGENTS.d/git-fixture-isolation.md)
