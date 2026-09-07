- **`setup.py` counts as a dependency manifest for the bot-PR gate exemption**
  (ADR-1152). Renovate's `pip_setup` manager edits `install_requires` in
  `setup.py`, so a version bump applied across all Python managers touches it
  alongside the `pyproject.toml` / `requirements.txt` files. `setup.py` was
  missing from the manifest allowlist in
  `scripts/ci/classify-dependency-pr.sh`, so those PRs were classified as
  source-touching and blocked by the Deliverables Checklist and Doc-Substance
  gates — a bot PR changing one version string could not merge (PR #1380). The
  allowlist now covers `setup.py` and `setup.cfg`, with regression tests for
  both the exemption and the case that must stay gated (a source edit riding
  along with the manifest change).
