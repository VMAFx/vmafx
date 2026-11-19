---
paths:
  - scripts/ci/check_licence_metadata.py
  - scripts/ci/tests/test_check_licence_metadata.py
invariant: Every published production image stage's OCI licence label equals the union of its manifest record's licence-bearing components.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Docker image licence labels (ADR-1250, ADR-1699)

`check_licence_metadata.py` compares the effective
`org.opencontainers.image.licenses` label of each published stage in the
production Dockerfiles with the union of every licence-bearing component
in its `tools/rc1-tester/image/licensing.json` record. A component written
`{"from": <artifact>, "id": <component>}` is resolved through the
reference chain; a cycle is an error, not a pass.

The label is read per stage: labels declared before the final `FROM` are
not inherited by the published stage, while labels on a parent stage are.
`IMAGE_TARGET_STAGES` names the published stages of each Dockerfile.

`licence` values that are not a single SPDX token (the NVIDIA CUDA EULA
text with its Attachment A sentence) stay in the notices and are not part
of the OCI label. All other single-token `licence` and `licences` entries
are in the union.

**Invariant**: the effective label is an `AND` set equal to the resolved
record. Missing, extra, absent and lowercase labels all fail. Shared
fixture roots without an `artifacts` record are skipped so unrelated
checks can keep their reduced layout; the repository root may not omit
that record, and a path listed in `IMAGE_LICENCE_LABELS` whose Dockerfile
is missing is reported when the manifest has the record.

Tests: `scripts/ci/tests/test_check_licence_metadata.py` covers a matching
tree, a missing licence, an extra licence, no label, a lowercase label,
a fixed component licence, a label before the published `FROM`, a label
inherited from the parent stage, a `from` cycle and a missing Dockerfile.
