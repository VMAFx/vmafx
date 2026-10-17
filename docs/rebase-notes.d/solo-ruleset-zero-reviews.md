## Solo ruleset: zero reviews, pull-request bypass (2026-10-14)

- `.github/repository-security-policy.json` declares ruleset 22587111 as the forge reports it: review count 0,
  no last-push approval, bypass mode `pull_request` (ADR-3313). A rebase keeps these values; the security
  checker compares them with the live ruleset.
- `.config/lint-exceptions.d/solo-repository-bypass-mode.toml` is deleted. A branch that still carries it fails
  the checker as a stale entry; drop it when rebasing.
- No upstream file is touched.
