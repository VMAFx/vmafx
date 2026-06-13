<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1074: Helm chart values completeness — missing knobs and schema gaps

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `helm`, `k8s`, `bug`

## Context

A systematic audit of `deploy/helm/vmafx/values.yaml` against `values.schema.json`
and every template under `deploy/helm/vmafx/templates/` revealed four completeness
gaps that cause either silent misconfiguration or a hard `helm lint` failure when
users supply standard Helm override keys:

1. **`nameOverride` / `fullnameOverride` absent from both values.yaml and schema.**
   The helpers in `_helpers.tpl` read `{{ .Values.nameOverride }}` and
   `{{ .Values.fullnameOverride }}` (lines 10, 18–19, 21).  Because the root schema
   carries `"additionalProperties": false`, any user who sets `--set nameOverride=foo`
   gets an immediate `helm lint` / `helm install` schema validation failure — the
   canonical Helm convention is completely blocked.

2. **StatefulSet inline PVC size hardcoded to `1Gi`.**
   `templates/statefulset.yaml` line 145 emits `storage: 1Gi` literally.  The
   StatefulSet `state` volume holds MCP server session state at `/var/lib/vmafx`;
   operators with large or long-lived sessions cannot tune this without forking
   the template.

3. **`node.metricsPort` absent from values — 9090 hardcoded in three places.**
   `templates/node.yaml` lines 82 and 188 and `templates/networkpolicy.yaml`
   line 255 all hardcode port 9090.  Operators who need to run vmafx-node alongside
   another Prometheus-scraped workload on the same port (a common cluster constraint)
   have no override path.  The NetworkPolicy allow-rule silently opens the wrong port
   if the node binary is reconfigured externally.

4. **`service.extraPorts` items schema is untyped.**
   The schema emits `"extraPorts": { "type": "array" }` with no `items` definition.
   A malformed port object (missing `name`, non-integer `port`) passes `helm lint`
   silently and only fails at `kubectl apply` time with an opaque apiserver error.

## Decision

Fix all four gaps:

1. Add `nameOverride: ""` and `fullnameOverride: ""` to values.yaml and to the root
   `properties` block in values.schema.json (both `type: string`).
2. Add `statefulSet.statePVCSize: "1Gi"` to values.yaml; wire it into the
   `volumeClaimTemplates[0].spec.resources.requests.storage` field in
   `templates/statefulset.yaml`; add the key to the `statefulSet` properties block
   in the schema with a Kubernetes-quantity `pattern` constraint.
3. Add `node.metricsPort: 9090` to values.yaml; replace all three hardcoded `9090`
   occurrences in templates with `{{ .Values.node.metricsPort | default 9090 }}`;
   add the key to the `node` properties block in the schema with
   `minimum: 1, maximum: 65535`.
4. Replace the bare `"extraPorts": { "type": "array" }` in the `service` schema
   with an `items` object that requires `name` and `port` and validates `protocol`
   as an enum.

No template logic changes beyond the four targeted substitutions; no new required
fields are introduced; default values preserve existing rendered output byte-for-byte.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave nameOverride/fullnameOverride out of schema; set `additionalProperties: true` at root | No schema change needed | Loses all type-safety on misspelled top-level keys; ADR-1047 explicitly chose strict root schema | Not chosen; strict root `additionalProperties: false` is load-bearing |
| Expose operator ports (8081/8082) as values too | Consistent with node.metricsPort pattern | Operator port conflicts are rare and the operator args would need re-wiring beyond just the port | Deferred; operator port exposure is a separate concern with lower urgency |
| Validate extraPorts items with `unevaluatedProperties: false` | Stricter | JSON Schema 2020-12 `unevaluatedProperties` is not supported by helm's validator (uses draft-07 semantics internally) | Not chosen; `required` + `properties` is the portable subset |

## Consequences

- **Positive**: `helm lint` and `helm install --dry-run` now catch all four classes of
  misconfiguration.  Standard Helm `nameOverride` / `fullnameOverride` conventions work
  without schema rejection.  StatefulSet PVC size and node metrics port are documented
  operator knobs.
- **Negative**: Users who supplied a custom `extraPorts` item without a `name` field
  will now receive a schema validation error.  This is intentional — a nameless port
  is invalid Kubernetes YAML.
- **Neutral / follow-ups**: The `statePVCSize` default `"1Gi"` and `metricsPort`
  default `9090` preserve current rendered output; no migration needed for existing
  installs.

## References

- `deploy/helm/vmafx/values.yaml`, `deploy/helm/vmafx/values.schema.json`.
- `deploy/helm/vmafx/templates/statefulset.yaml` (line 145 — hardcoded `1Gi`).
- `deploy/helm/vmafx/templates/node.yaml` (lines 82, 188 — hardcoded `9090`).
- `deploy/helm/vmafx/templates/networkpolicy.yaml` (line 255 — hardcoded `9090`).
- `deploy/helm/vmafx/templates/_helpers.tpl` (lines 10, 18–21 — `nameOverride`/`fullnameOverride`).
- ADR-1047: preceding Helm schema correctness fixes (R9 batch).
