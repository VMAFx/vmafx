- **Rewrote the Renovate agent note for the GitHub App setup.**
  `.github/AGENTS.d/renovate-bot.md` still described the self-hosted Renovate
  workflow that ADR-0387 removed on 2026-05-10: the `invariant:` named the
  `RENOVATE_TOKEN` repository secret and the body pointed at
  `workflows/renovate.yml`, which no longer exists. The note now describes the
  Mend Renovate GitHub App that reads the in-tree `renovate.json` and opens
  grouped dependency-update PRs, matching the operator playbook in
  `docs/development/dependency-bot.md`, keeps the upstream-sync guidance, and
  the generated `.github/AGENTS.md` invariant row is recompiled to the same
  text.
