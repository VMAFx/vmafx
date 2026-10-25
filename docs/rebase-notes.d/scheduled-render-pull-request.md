## Scheduled render pull request (2026-10-25)

- `.github/workflows/docs-render-pr.yml` (new) and `scripts/ci/open-render-pr.sh` (new) open the
  ADR-3295 render-only pull request from branch `chore/docs-render-auto`
  ([ADR-3373](adr/3373-scheduled-render-pull-request.md)).
- `scripts/ci/check_render_on_master.py` gains `--open-after-hours`, which prints
  `open=`, `owed_since=` and `owed_hours=` lines for the workflow; without the flag it behaves as
  before.
- No upstream file is touched; an upstream sync has nothing to keep.
