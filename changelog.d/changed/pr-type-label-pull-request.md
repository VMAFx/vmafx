- **The pull-request type labeller runs on `pull_request`
  ([ADR-3376](docs/adr/3376-pr-type-label-pull-request.md)).** GitHub's default
  Actions event policy blocks `pull_request_target` on public repositories from
  2026-12-02, so `pr-type-label.yml` no longer uses it. A pull request from a
  fork gets a read-only token there: the job leaves a notice and a maintainer
  adds the `type:*` label by hand.
