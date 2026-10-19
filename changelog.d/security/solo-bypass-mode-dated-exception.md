- **The maintainer's ruleset bypass at mode `always` is now a declared exception with an end date.**
  The standard for a single-maintainer repository is mode `pull_request`; the merge train still lands
  by direct push, so `.config/lint-exceptions.d/solo-repository-bypass-mode.toml` declares the
  deviation until 2027-01-09 (ADR-3273). `scripts/dev/check_repository_security.py` fails when the
  policy file declares another mode than `pull_request` without an unexpired entry, and when the
  entry outlives the mode. No ruleset or protection setting changes.
