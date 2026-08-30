- **Pre-push PR-body deliverables validator.** New
  `scripts/ci/validate-pr-body.sh` standalone CLI and
  `scripts/git-hooks/pre-push` git hook (installed by
  `make hooks-install`) run the same ADR-0108 deep-dive-checklist
  parser as `.github/workflows/rule-enforcement.yml` against the
  branch's open PR body before pushing. Closes the loop on the
  multi-retry feedback cycle the strict parser caused on PRs #461,
  #438, #470, #473, #486, #511, #468, and #526. Bypass via
  `git push --no-verify`. See `docs/development/pr-body-validator.md`.
