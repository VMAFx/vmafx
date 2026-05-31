- Fix two latent bugs in the upstream-mirror `compat/python-vmaf/` tree
  (ADR-0955):
  - `tools/scanf.py::makeFormattedHandler.applyWidth` had an inverted
    `if width is None` check. Implicit-width converters (`%d`, `%f`,
    `%s`, `%x`) crashed inside `CappedBuffer` with `TypeError`, and
    explicit-width converters (`%5d`) silently ignored the cap. The
    branches are now swapped to the intended semantics.
  - `__init__.py::ProcessRunner.run` used `env.setdefault("LC_ALL",
    "C")` / `env.setdefault("LANG", "C")`, which is a no-op when the
    keys already exist. A parent shell with `LANG=de_DE.UTF-8` defeated
    the override and subprocess error messages came back
    locale-translated, breaking downstream assertions that grep for
    English phrases. Replaced with unconditional assignment.
  - Both fixes carry regression tests in
    `python/test/python_harness_scanf_locale_bugs_test.py` and a
    rebase-notes entry so future upstream syncs preserve them.
