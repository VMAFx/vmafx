- Removed a `.venv` symlink that #1231 accidentally committed. It pointed at
  an absolute path on the author's machine, so on every other checkout it
  resolved to itself; because git treats ignored paths as disposable, a plain
  `git pull` then replaced any real `.venv` with a self-referential loop that
  broke every PATH-searched command (`env`, `nohup`, hook shebangs, semgrep).
  `.gitignore` now also carries `.venv*` without a trailing slash so symlinks
  and files are excluded, not only directories, and a new gate
  (`scripts/ci/check-no-tracked-venv.sh`, pre-commit + `make lint-sh`) refuses
  any tracked virtualenv path. **If you pulled master between 2026-09-03 20:10
  and this fix, your `.venv` is gone: delete the symlink and recreate the venv.**
