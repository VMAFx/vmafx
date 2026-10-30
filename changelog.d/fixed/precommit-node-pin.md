- Local Git hooks: pre-commit's node hooks (`markdownlint-cli2`) now run on
  Node.js 24.22.1, the `NODE_VERSION` in `build-config.env`. Unpinned,
  pre-commit on Windows built them at the newest entry of the Node.js index,
  and a prerelease there (`v27.0.0-alpha.0.0.0`) made nodeenv fail and the
  `Windows Lefthook Pre-Commit` job red on master.
  `scripts/ci/check-workflow-versions.py` now refuses a pre-commit or
  `setup-node` pin that differs from `NODE_VERSION`
  ([pre-commit hooks](docs/development/pre-commit-hooks.md#node-hooks-run-on-a-pinned-nodejs-release)).
