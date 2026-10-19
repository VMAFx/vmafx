- CI: the `Tidy Changed` job now lints a changed C-only header (one that
  only C translation units include) through the including units that have a
  compile command in the CPU build, as the `Tidy SYCL` job already does.
  Linted on its own, such a header borrowed a C++ unit's command without the
  unit's include paths and failed with `file not found` and undeclared
  identifiers (`scripts/ci/tidy_changed_route.py`).
