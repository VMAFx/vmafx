- **docs**: bulk-flip ADR Status `Proposed` → `Accepted` for 13 ADRs whose
  implementing PRs landed during the 2026-05-06 merge train (ADRs 0302
  / 0303 / 0304 / 0305 / 0307 / 0308 / 0309 / 0311 / 0313 / 0314 / 0316
  / 0317 / 0319). Per ADR-0028 / `docs/adr/README.md`, ADRs flip to
  Accepted once the deliverable lands; the train moved faster than the
  per-ADR Status bumps could keep up. ADR-0313's Status row was using
  table-format (`| Status | Proposed |`) instead of the bullet-format
  (`- **Status**: Proposed`) the other ADRs use, so the bulk sed missed
  it; fixed inline.
