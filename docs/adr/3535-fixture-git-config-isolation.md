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

Extend `unisolated_git()` through the existing
`test_no_script_creates_or_configures_a_repository_unisolated` method and
`git-fixture-isolation-static` hook. Keep both new checks and their replay cases
on that existing entry point.

Use a text allow-list for Python environment filters. Tokenization locates
direct comprehensions and `filter` lambdas over `os.environ`, ignores comments
and stored source strings, and retains the original condition for diagnostics.
An exclusion of the complete `GIT_` prefix must consist solely of a negated
`startswith` call on the iterated name. A tuple beginning with `GIT_` may
exclude additional prefixes. Additional conditions fail with the source path,
line and condition text. Selection of planted `GIT_CONFIG_KEY_*` values does not
claim to exclude the complete prefix and remains valid.

For Python fixtures that create or configure repositories and carry an isolation
marker, require explicit assignments setting `GIT_CONFIG_GLOBAL` to the null
device and either `GIT_CONFIG_SYSTEM` to the null device or
`GIT_CONFIG_NOSYSTEM` to `1`. Accept dictionary entries, keyword arguments,
dictionary updates and subscript assignments. Fixture scope includes existing
test-script names, files under a `tests/` directory and Python basenames
containing `fixture`. Preserve all inherited-variable exclusions and both
configuration barriers when updating helpers. Reuse imported environment helpers
where available.

Apply the missing keys to the fifteen fixture files. Keep production licensing
behavior unchanged: its temporary archive-fetch repositories are outside the
fixture configuration rule. Normalize two other existing Python filters to meet
the complete-condition allow-list while preserving their removal of additional
variables.

Node and Go retain the existing isolation-marker check. Additional re-admission
analysis is deferred: callback predicates and imperative environment mutation
need language-specific extraction beyond this Python text check. The agent
invariant page `scripts/ci/AGENTS.d/git-fixture-isolation.md` records this
coverage limit.

## Alternatives considered

| Option | Advantages | Limitations | Outcome |
| --- | --- | --- | --- |
| AST parse | Precise expression structure and source locations | Requires parseable complete Python and more structural policy machinery | Deferred |
| Text allow-list with token boundaries | Small explicit accepted language; ignores stored test source; actionable conditions | Does not prove data flow or helper behavior | Chosen |
| Runtime-only sentinel | Detects observable writes to a temporary caller repository | Cannot cover every product script or pytest-only fixture; configuration inheritance need not damage the sentinel | Retained as complementary coverage |

## Consequences

- Fixture commits ignore caller signing defaults and all inherited `GIT_*`
  variables.
- Extra Boolean clauses fail even when they only exclude more keys; separate
  operations preserve additional removals.
- The existing static test replays rejected filters, missing keys, accepted
  assignment styles, tuple prefixes, caller-poison selection and a product-file
  exemption. A temporary copy of `test_ci_impact.py` fails the shared
  `unisolated_offenders()` check after re-admission is planted and passes after
  restoration; the check runs on the copy directly, so the test never re-enters
  itself.
- Configuration assignments are checked textually within each file. Alias
  handling, interprocedural data flow and assignment-to-subprocess association
  remain outside this check.

## References

- [Static check and replay
  cases](../../scripts/ci/test_git_fixture_isolation.py)
- [Shell environment helper](../../scripts/lib/clean-git-env.sh)
- [Hook documentation](../development/pre-commit-hooks.md)
- [Agent invariant](../../scripts/ci/AGENTS.d/git-fixture-isolation.md)
