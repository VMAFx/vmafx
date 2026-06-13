- **docs(adr):** Status-field drift sweep across all 630 ADRs. Flipped
  ADR-0573 from `Accepted` to `Superseded by ADR-0738` (CUDA 13.2 → 13.3
  container bump fully supersedes the earlier toolchain pin). Normalised
  the `Status` field of ADR-0105 / ADR-0106 / ADR-0107 from the malformed
  `Supersedes [ADR-NNNN]` (the supersedes relationship was inhabiting
  the Status slot) to `Accepted` with a dedicated `**Supersedes**:` line
  immediately below. Partial-clause supersedes that survive: ADR-0257
  (only its "Negative consequence about content-independent saliency"
  is superseded by ADR-0286), ADR-0537 (only its "places=3 acceptable
  as follow-up" clause is superseded by ADR-0566 / ADR-0554) — both
  remain `Accepted` because the bulk of each ADR's decision still
  stands. Zero orphan `Superseded` chains found (every ADR marked
  Superseded has a successor identified). Post-sweep status counts:
  571 Accepted, 31 Proposed, 19 missing-Status (older ADRs predating
  the field-required template), 8 Superseded, 1 Draft.
