- **`make lint-tools` installed an older ruff than the pre-commit hook ran.**
  The `Makefile` promised its ruff and black pins were identical to
  `.pre-commit-config.yaml`, but Renovate only ever raised the hook side: the
  hook reached ruff 0.16.8 while `make lint-tools` installed 0.16.5 and one
  install hint still named 0.15.17. The pins are aligned, the install hints use
  the variables, `scripts/ci/check-workflow-versions.py` now fails when the two
  files disagree or a recipe spells the version as a literal, and Renovate
  raises the `Makefile` pins in the same pull request as the hook revisions.
