## Render-only pull request replaces the direct render push (2026-10-13)

- `scripts/ci/deliverables-check.sh` accepts a diff of rendered paths only when
  `make docs-render-check` passes on its head (ADR-3295); keep the detached
  worktree check and the refusal of a rendered file next to any other file.
- The master push step of the Docs job in
  `.github/workflows/lint-and-format.yml` runs
  `scripts/ci/check_render_on_master.py`, not `make docs-render-check`.
- Tests: `scripts/ci/tests/test_deliverables_render_only.py`,
  `scripts/ci/tests/test_check_render_on_master.py`.
