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
`IMAGE_STAGE_RECORDS` names the published stages of each Dockerfile and the
manifest record each stage must match; a listed stage that the Dockerfile
does not have is reported, and there is no last-stage fallback.

`licence` values that are not a plain `AND` list of SPDX terms stay in the
notices and are not part of the OCI label. `NOTICE_ONLY_COMPONENTS` names
each such component by id and records why; a complex value from any other
component is a `ModelError`, not a silent omission. A
`<id> WITH <exception>` term is kept as one label term when the component is
not named in that table. All `licences` list entries are in the union.

**Invariant**: the effective label is an `AND` set equal to the resolved
record. Missing, extra, absent and lowercase labels all fail. Shared
fixture roots without an `artifacts` record are skipped so unrelated
checks can keep their reduced layout; the repository root may not omit
that record, and a path listed in `IMAGE_STAGE_RECORDS` whose Dockerfile
is missing is reported when the manifest has the record.

Tests: `scripts/ci/tests/test_check_licence_metadata.py` covers a matching
tree, a missing licence, an extra licence, no label, a lowercase label,
a fixed component licence, a label before the published `FROM`, a label
inherited from the parent stage, a `from` cycle, a missing Dockerfile,
a renamed published stage, a notice-only component and an unlisted complex
licence.
