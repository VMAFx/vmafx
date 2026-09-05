- `scripts/ci/check-no-tracked-venv.sh` no longer flags files whose basename merely
  starts with `venv` (its optional-dot pattern matched
  `changelog.d/fixed/venv-recipe-docs.md` and reddened #1282's required
  Pre-Commit check); it now matches real virtualenv entries (`.venv*`, `venv`,
  `.virtualenv`, `pyvenv.cfg`) and files inside such directories, with a test.
