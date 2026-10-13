- **The render lands as its own pull request
  ([ADR-3295](docs/adr/3295-render-only-pull-request.md)).** The rendered
  changelog, ADR index and rebase notes land after each landing batch through a
  render-only pull request instead of a direct push to `master`. The
  deliverables gate accepts a pull request that touches only rendered files
  when its head passes `make docs-render-check`, and the master push runs
  `scripts/ci/check_render_on_master.py`, which allows an owed render for up to
  24 hours.
