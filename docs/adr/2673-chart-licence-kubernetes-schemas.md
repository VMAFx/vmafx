<!-- markdownlint-disable MD013 MD060 -->
# ADR-2673: the Helm chart declares EUPL-1.2 AND Apache-2.0 because its values schema embeds Kubernetes type schemas

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: maintainer, agent
- **Tags**: license, compliance, helm, k8s, fork-local

## Context

[ADR-1699](1699-root-licence-files-eupl.md) has every package manifest declare
the licences of exactly the files the package ships, and gave the Helm chart
`artifacthub.io/license: EUPL-1.2`. All of the chart's own files were EUPL-1.2
then, and ADR-1699 turned down an AND expression for that annotation because
Artifact Hub asks for a single SPDX identifier there
(`docs/helm_annotations.md`: "It must be a valid SPDX identifier", read again on
2026-10-08).

[ADR-2350](2350-cloud-native-platform.md) D13 generates the chart's
`values.schema.json` from the platform definition, and the maintainer chose to
check the values the chart copies into pod specs with the Kubernetes types of
the minimum supported minor. The schema now carries 89 type schemas from the
OpenAPI v3 specification of `kubernetes/kubernetes` v1.26.15 (Apache-2.0,
"The Kubernetes Authors"), with descriptions and extensions removed but
otherwise as published. The chart's own files are therefore EUPL-1.2 and
Apache-2.0, and `scripts/ci/check_licence_metadata.py` refuses a single
`EUPL-1.2`. Apache-2.0 also asks that whoever redistributes the work passes
on a copy of the licence and the attribution.

## Decision

The chart declares `artifacthub.io/license: EUPL-1.2 AND Apache-2.0`, the
licences its own files carry; this replaces the chart row of ADR-1699. The
chart ships `THIRD-PARTY-NOTICES.txt`, which names the Kubernetes types in
`values.schema.json`, their source release and copyright holder, and carries
the Apache-2.0 text, so a packaged chart carries the attribution with it.
`check_licence_metadata.py` reads an SPDX AND expression from
`artifacthub.io/license` and compares it with the licences `REUSE.toml` gives
the chart's files, as it does for every other manifest. The chart is not
published on Artifact Hub today; if it ever is, this decision is revisited
against Artifact Hub's single-identifier rule.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Declare `EUPL-1.2 AND Apache-2.0` and ship the Apache-2.0 notice (chosen) | The declaration states the licences the files carry; attribution travels with the chart | Not a single SPDX identifier as Artifact Hub asks | The maintainer's choice (Q-277); the chart is not on Artifact Hub |
| Treat the type schemas, without their descriptions, as interface data that carries no Apache-2.0 terms | The chart stays `EUPL-1.2` | A legal judgement about copyright in API schemas the project cannot settle; the subset file would still be Apache-2.0 | Not taken (Q-277) |
| Leave the Kubernetes types out of the schema | No licence change | Gives up the validation the maintainer asked for (Q-276) | Not taken (Q-277) |
| Keep `EUPL-1.2` and record the Apache-2.0 part only in `REUSE.toml` | Artifact Hub's rule kept | The manifest would name fewer licences than the files carry, which ADR-1699 forbids | Contradicts ADR-1699's rule for every manifest |

## Consequences

- **Positive**: the chart's declaration matches its files; the Apache-2.0
  attribution ships in the packaged chart.
- **Negative**: `artifacthub.io/license` is an expression, not one
  identifier; Artifact Hub may show it as it is or not at all.
- **Neutral / follow-ups**: `REUSE.toml` records
  `deploy/helm/vmafx/values.schema.json` as `EUPL-1.2 AND Apache-2.0`,
  `api/kubernetes/openapi-subset.json` and the notice as `Apache-2.0`;
  `docs/credits.yaml` lists the Kubernetes specification; `docs/licensing.md`
  shows the new value. ADR-1699's status line points here. Revisit when the
  chart is published on Artifact Hub, or when the minimum Kubernetes minor
  changes the copied types.

## References

- [ADR-1699](1699-root-licence-files-eupl.md) — the manifest rule and the
  chart row this changes.
- [ADR-2350](2350-cloud-native-platform.md) D13 — the generated values schema.
- Artifact Hub, `docs/helm_annotations.md`, `artifacthub.io/license`, read
  2026-10-08.
- `Q-276` (popup, 2026-10-08): "Include in WP4a: reference upstream
  Kubernetes types from the minimum supported minor; stricter validation
  documented with a compatibility note".
- `Q-277` (popup, 2026-10-08): "A: artifacthub.io/license 'EUPL-1.2 AND
  Apache-2.0'; new ADR amending ADR-1699's chart row; checker accepts the AND
  expression; credits entry and notice for the Kubernetes schemas".
