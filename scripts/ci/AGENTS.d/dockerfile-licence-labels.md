---
paths:
  - scripts/ci/check_licence_metadata.py
  - scripts/ci/tests/test_check_licence_metadata.py
invariant: Every production image's OCI licence label equals its manifest vmafx-binaries set.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Docker image licence labels (ADR-1250, ADR-1699)

`check_licence_metadata.py` compares `org.opencontainers.image.licenses`
on the production Dockerfiles with the `vmafx-binaries` component of
their `tools/rc1-tester/image/licensing.json` record. A component written
`{"from": <artifact>, "id": <component>}` is resolved through the
reference chain; a cycle is an error, not a pass.

**Invariant**: the effective label is an `AND` set equal to the resolved
record. Missing, extra, absent and lowercase labels all fail. Shared
fixture roots without an `artifacts` record are skipped so unrelated
checks can keep their reduced layout; the repository root may not omit
that record, and a path listed in `IMAGE_LICENCE_LABELS` whose Dockerfile
is missing is reported when the manifest has the record.

Tests: `scripts/ci/tests/test_check_licence_metadata.py` covers a matching
tree, a missing licence, an extra licence, no label, a lowercase label,
a `from` cycle and a missing Dockerfile.
