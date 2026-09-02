<!-- markdownlint-disable MD013 MD060 -->
# ADR-0955: Fix two latent bugs in upstream-mirror `compat/python-vmaf/` (scanf width handling + ProcessRunner locale forcing)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: @Lusoris
- **Tags**: python-harness, upstream-mirror, fix, locale, scanf

## Context

The upstream-mirror tree `compat/python-vmaf/` carries two latent bugs
inherited from Netflix/vmaf that the fork's tooling tripped over. PR #454
(Python harness coverage push) surfaced both during scaffolding.

**Bug 1 — `tools/scanf.py::makeFormattedHandler.applyWidth`.** The width
guard was inverted:

```python
def applyWidth(handler):
    if width is None:                          # WRONG branch
        return makeWidthLimitedHandler(handler, width, ignoreWhitespace=True)
    return handler
```

For implicit-width converters (`%d`, `%f`, `%s`, `%x`), this passed
`width=None` into `makeWidthLimitedHandler` → `CappedBuffer(buffer,
None, ...)`, which detonated with `TypeError: '<' not supported between
instances of 'int' and 'NoneType'` on the first byte read. For
explicit-width converters (`%5d`), the cap was silently dropped — `%5d`
read an unbounded run of digits. Manual verification with the
pre-fix tree reproduces both symptoms.

**Bug 2 — `__init__.py::ProcessRunner.run`.** Locale forcing used
`env.setdefault("LC_ALL", "C")` / `env.setdefault("LANG", "C")`.
`setdefault` is a no-op when the key already exists, so on hosts
with `LANG=de_DE.UTF-8` (this developer's primary box) the override
never took effect — subprocess error messages came back as
"Kommando nicht gefunden" instead of "command not found", and
downstream test assertions that grep for canonical English phrases
failed in confusing ways. The fix is unconditional assignment.

Both files live under `compat/python-vmaf/` — the upstream-mirror
tree — so the fix has to be carried as a fork delta with a rebase
note that future Netflix syncs preserve it.

## Decision

Apply both fixes in-place in `compat/python-vmaf/`, ship regression
tests under `python/test/python_harness_scanf_locale_bugs_test.py`
(separate file from PR #454's `python_harness_coverage_test.py` to
avoid merge conflicts), and document the fork delta in
`docs/rebase-notes.md` so a future upstream sync that touches either
file preserves the bug fixes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix in-place + rebase note (chosen) | Bug stays fixed; clear paper trail; standard fork-delta pattern | Adds a rebase-impact entry | The bugs are real and reproducible; carrying them is worse than the rebase cost |
| Wait for upstream fix | Zero fork delta | Bug 1 has been latent in upstream for years (no PR open); Bug 2 only matters on non-C-locale hosts (Netflix CI is English) — upstream is unlikely to prioritise | Both bugs actively break this fork's tooling today |
| Wrap `scanf`/`ProcessRunner` in a fork-local shim | No upstream-mirror edit | Doubles the surface area; the shim has to be kept in sync with upstream changes; defeats the point of having `compat/` mirror upstream | The fix is 4 lines per file; a shim is more code to maintain |
| Fold the test into `python_harness_coverage_test.py` (PR #454) | One coverage file | Creates a merge conflict with an in-flight PR; couples unrelated work | Keeping separate files makes both PRs cleanly mergeable in any order |

## Consequences

- **Positive**: Implicit-width `%d` / `%f` / `%s` / `%x` in
  `scanf.sscanf` now works; explicit-width caps are honoured;
  subprocess error messages from `ProcessRunner` are uniformly
  English regardless of host locale.
- **Negative**: Adds two fork-deltas against upstream Netflix/vmaf
  in files that future upstream syncs may touch. Mitigated by
  the rebase note (`docs/rebase-notes.md`) and the AGENTS.md
  invariant pointer.
- **Neutral / follow-ups**: When porting an upstream commit that
  re-touches `compat/python-vmaf/tools/scanf.py::applyWidth` or
  `compat/python-vmaf/__init__.py::ProcessRunner.run`, verify
  the fork's fixes are preserved or that upstream has independently
  adopted equivalent fixes.

## References

- PR #454 — Python harness coverage push (surfaced both bugs).
- `compat/python-vmaf/tools/scanf.py:638-664` — `makeFormattedHandler`.
- `compat/python-vmaf/__init__.py:46-65` — `ProcessRunner.run`.
- `docs/rebase-notes.md` — fork-delta ledger for upstream syncs.
- Source: `req` — user-flagged latent bugs in PR #454 follow-up.
