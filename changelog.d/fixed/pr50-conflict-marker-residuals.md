- **PR #50 conflict-marker residuals — fully closed.** Two files still
  carried unresolved `<<<<<<< HEAD … =======` blocks from commit
  `24bb5daf89` (PR #50 disaster): `.semgrepignore` (4 blocks; semgrep
  was silently ignoring the resolution lines as comments) and
  `docs/backends/sycl/overview.md` (1 block; mkdocs picked up the marker
  text as prose). HEAD side kept in both — the `core/` paths from the
  post-ADR-0700 rename. `git grep '<<<<<<'` now returns empty across the
  tracked tree. `docs/state.md` row for T-CI-CONFLICT-MARKERS-PR50 moved
  to "Recently closed".
