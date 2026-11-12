---
paths:
  - .github/workflows/readme-badges.yml
invariant: Render job on master only; both checkouts at github.sha; publish job contents: write only; no workflow_run trigger.
---
# README badges workflow (ADR-3519)

`.github/workflows/readme-badges.yml` renders + publishes README status
badges onto `badges` branch. Script rules, design check, carry:
[scripts/ci/AGENTS.d/readme-badges.md](../../scripts/ci/AGENTS.d/readme-badges.md).
Human guide: `docs/development/readme-badges.md`.

- Guard: render job `if:` = policy repository + `https://github.com` +
  `github.ref == 'refs/heads/master'`. Publish job `needs: render`, so
  skipped render skips publish. Manual run on other branch -> nothing runs;
  publish token never meets branch copy of workflow. Names come from
  `.github/repository-security-policy.json`; `test_readme_badges.py` keeps
  them equal.
- Checkout: both jobs `ref: ${{ github.sha }}`, `persist-credentials: false`.
  Never `ref: master`: master push between jobs hands publisher other code
  than renderer ran.
- Rights: top level `permissions: {}`; render job `actions: read` +
  `contents: read`; publish job `contents: write` only. No other write scope.
- Trigger: `schedule` (minutes 7, 22, 37, 52) + `workflow_dispatch`. No
  `workflow_run` (zizmor dangerous trigger; fork branch named `master`
  matches `branches` filter).
- Ref: publish writes `refs/heads/badges` only, `force: false`;
  GitHub-signed commit required.
