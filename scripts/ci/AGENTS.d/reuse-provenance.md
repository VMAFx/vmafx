---
paths:
  - REUSE.toml
  - scripts/ci/tests/test_reuse_compliance.py
invariant: `reuse lint` green is not provenance: keep exact overrides for inherited, ported, no-CLA and FFmpeg-patch files.
---
<!-- markdownlint-disable MD013 MD060 -->
# REUSE coverage and provenance (BUG-003 / ADR-1250)

`reuse lint` proves metadata coverage and syntax, not ownership or licence
provenance. root `REUSE.toml` EUPL-1.2 default applies only to work already
classified as fork-authored. Preserve exact overrides for inherited and renamed
Netflix files, ports and twins, no-CLA outside contributions (including
append-only aggregates), third-party artefacts, and FFmpeg patches. blanket
EUPL-1.2 annotation can be 100% REUSE-compliant while falsely relicensing those
files and is therefore forbidden.

When upstream sync, rename, outside contribution, aggregate edit, or FFmpeg
patch changes one of those sets, update mapping and
`tests/test_reuse_compliance.py` together. Classify each FFmpeg patch against
exact configured upstream tag and every source unit it changes. Re-run
rename-aware history audit described in
[`docs/research/bug-003-reuse-provenance-audit-2026-09-24.md`](../../../docs/research/bug-003-reuse-provenance-audit-2026-09-24.md);
zero missing metadata is necessary but not sufficient evidence.
