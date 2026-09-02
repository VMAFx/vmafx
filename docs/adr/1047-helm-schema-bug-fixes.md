<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1047: Helm chart schema and values.yaml correctness fixes (R9 batch)

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `helm`, `k8s`, `bug`

## Context

The R9 bug hunt identified four correctness gaps in the Helm chart that cause silent
misconfiguration:

1. `storage` key defined in `values.schema.json` (with `mode` and `rclone` sub-fields)
   but absent from `values.yaml`. A user setting `storage.mode=rclone` at install time
   would pass schema validation but produce an undefined-key structure in the templates.
2. Three top-level keys — `networkPolicy`, `auth`, and `otelCollector` — are documented
   in `values.yaml` but absent from the schema. Typos in those blocks are silently
   accepted at `helm lint`/`install` time.
3. `gpu.count` carries `"minimum": 0`, making a value of 0 with `gpu.enabled: true`
   schema-valid. Every vendor device plugin treats 0 units as a silent no-op, so a
   misconfigured chart deploys a pod that requests no GPU and runs CPU-only without
   any warning.
4. `gpu.enabled` is not listed in the `gpu` object's `required` array; a user who
   deletes the key gets no validation error from `helm lint`.

## Decision

Fix all four gaps in a single atomic commit:

1. Add `storage` with correct defaults (`mode: "http-serve"`, `rclone.config: ""`)
   to `values.yaml` so the key exists and the documented default is explicit.
2. Add `networkPolicy`, `auth`, and `otelCollector` to `values.schema.json` with
   `additionalProperties: true` so nested sub-keys pass validation while still
   surfacing the top-level key in the validated surface.
3. Change `gpu.count.minimum` from `0` to `1`.
4. Add `"enabled"` to `gpu.required`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `additionalProperties: false` on `networkPolicy`/`auth`/`otelCollector` | Tighter validation | Would require exhaustive enumeration of every `allow.*` and `tenant.*` sub-field, high maintenance burden | Not chosen; `additionalProperties: true` catches top-level-key typos without requiring full enumeration |
| Conditional `if gpu.enabled then minimum:1` | More precise | JSON Schema 2020-12 `if/then` is supported but adds complexity | Simple minimum:1 is sufficient; count:0 with enabled:false is an operator error anyway |

## Consequences

- **Positive**: `helm lint` and `helm install --dry-run` will now catch the four classes
  of misconfiguration before they reach a cluster.
- **Negative**: Any user who was relying on `gpu.count: 0` as a valid value will receive
  a schema validation error. This is intentional — 0 GPUs is a misconfiguration.
- **Neutral / follow-ups**: The `storage` default value (`http-serve`) matches the
  existing controller behaviour; no template change required.

## References

- R9 bug hunt report (2026-06-04).
- `deploy/helm/vmafx/values.yaml`, `deploy/helm/vmafx/values.schema.json`.
