- **Code-quality:** Bulk-fixed mechanical CodeQL alerts in the Python tree:
  unused-import (10), use-of-exit-or-quit (7), file-not-closed (5),
  empty-except (2), unnecessary-pass (3), and one commented-out-code block.
  No behavioural change — `exit()` → `sys.exit()`, dropped open-without-`with`
  patterns wrapped in context managers, removed dead imports, added
  explanatory comments to typed `except KeyError: pass` bodies. C-side alerts
  (`integer-multiplication-cast-to-long`, `commented-out-code`,
  `declaration-hides-variable`, `large-parameter`,
  `poorly-documented-function`, `include-non-header`) deferred to follow-up
  PRs because all flagged C files are upstream-mirrored Netflix code where
  CLAUDE.md rule 12 (touched-file-cleanup) requires a full lint sweep that
  expands the diff well past the per-PR LOC budget.
