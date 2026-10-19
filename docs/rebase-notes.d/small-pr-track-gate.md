## Small-PR track in the deliverables gate (2026-10-08)

`community-smallpr`, [ADR-2461](adr/2461-small-pr-deliverables-track.md).
`scripts/ci/deliverables-check.sh` gained section 2c and a skip in the
six-item loop; the PR template gained a paragraph. Both are fork-authored; a
sync keeps the fork's side. `scripts/ci/tests/test_deliverables_small_pr.py`
guards the behaviour and reads the template and the sentinel guide.
