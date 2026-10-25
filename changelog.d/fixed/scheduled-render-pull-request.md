- CI: an hourly workflow (`docs-render-pr.yml`) now opens the render-only pull request for
  `CHANGELOG.md`, the ADR index and the rebase notes once a render has been owed on master for
  6 hours, and arms auto-merge. Until now nothing opened it, and master went red on the 24-hour
  render bound twice in two days (ADR-3373).
