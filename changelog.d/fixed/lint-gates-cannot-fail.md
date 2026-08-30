- `make format-check` — documented in `CLAUDE.md` §4 as the pre-commit / CI
  format gate — could not fail. All four of its steps ended in `|| true`, so
  clang-format, black, isort and shfmt violations were swallowed and the target
  always exited 0. `make lint-py` had the same defect for black and mypy, and
  additionally hid their output behind `2>/dev/null`.
- Lint and format tools now resolve from the project venv (`.venv/bin`) before
  the system `PATH`. Previously `make lint-py` looked up a bare `ruff` on the
  system `PATH` only; on a machine where ruff lives in the venv it printed
  "ruff not found; skipping" and exited 0, so the local gate reported success
  while CI's identical ruff check failed.
- A missing lint tool is now a hard error naming the install command, instead of
  a "skipping" message and a green exit. `mypy` stays advisory and is explicitly
  marked as such — it reports ~295 module-resolution errors that stop it before
  it type-checks anything, which is a mypy-configuration gap rather than type
  debt.
