- **The master ruleset policy file matches the live ruleset again.** `.github/repository-security-policy.json`
  declares no required approval, no last-push approval and the maintainer's bypass at mode `pull_request`,
  with every pull-request parameter the forge reports (ADR-3313, superseding ADR-3273). The bypass-mode
  exception entry is removed; `scripts/dev/check_repository_security.py` still fails on another mode without
  an unexpired entry and on a stale entry. The Scorecard Master Gate no longer fails on policy drift.
