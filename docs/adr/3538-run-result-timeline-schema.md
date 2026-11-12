<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3538: Run-result timeline schema closes objects and bumps the version per change

- **Status**: Accepted
- **Date**: 2026-11-12
- **Deciders**: luoye-cpu (author), lusoris (review)
- **Tags**: api, schema, versioning, interop

## Context

The run-result timeline schema (`api/run-result-timeline.schema.json`, #2452)
is the versioned report format a scoring run writes and the C ABI writer/reader
(the follow-up PRs of the same issue) serialises byte-for-byte. The skeleton
closes every object (`additionalProperties: false`), fixes the event vocabulary
to an enum and pins `schema_version` with `const: 1`.

The repository carries two disagreeing precedents for how such a versioned
document evolves:

- `docs/hardware-reports/report.schema.json` keeps closed objects and lists
  every known version in a `schema_version` enum, adding one per change: a
  version-1 reader refuses a whole document written against a newer version,
  even when the change was only an added member.
- [ADR-3311](3311-stream-metadata-field-list.md) keeps
  `api/stream-metadata-fields.json` append-only under version 1 and moves to
  version 2 only for a change that cannot be an addition.

`docs/development/agent-hard-rules.md` (item 8) asks for a decision record
when another engineer could reasonably have chosen differently; the skeleton
follows the first model and the schema description even said "evolves
additively under HISS-14" while its closed objects refuse additive extensions
under version 1 — the two statements contradict each other. The ADR resolves
the model, and with it whether `scoring_configuration` is a required section.

## Decision

1. **The timeline schema follows the closed-object model**: every object
   refuses unknown members, the event vocabulary is closed, and a change that
   adds a member or an event type takes a new schema version. A version-1
   reader refuses a version-2 document by name; it never silently drops
   unknown members. The schema evolves as additive supersets at version
   granularity: a newer version adds members and never removes, renames or
   retypes one (the HISS-14 discipline continues, one level up).
2. **`scoring_configuration` stays optional** in version 1 (not in the
   top-level `required`): a timeline can come from a CLI run that has no
   server-side scoring surface to record. `schema_version`, `provenance`,
   `frames`, `windows` and `events` stay required. When the section is
   present, its members follow the OpenAPI `ScoreOptions` component, and the
   acceptance test refuses members the component does not name.
3. **Non-finite results spell `null`**, one spelling for NaN, `+Inf` and
   `-Inf` alike, matching the CLI's JSON output
   (`core/src/output.cpp`, `json_write_frame_metric`). Telling NaN apart from
   ±Inf in the schema was rejected: the CLI does not distinguish them, one
   spelling across outputs is the HISS-19 rule, and a distinction would start
   with a CLI writer change and a schema version bump, not the other way
   around.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Field-registry document (Pelorus-style rows) instead of JSON Schema | the stream-metadata list works this way | no validator in the lock, the named-refusal acceptance does not map onto a validator error | JSON Schema draft 2020-12: two in-tree schemas already use it, the validator is pinned in the tooling-tests lock |
| Proto schema | strong cross-language typing | no proto surface anywhere in the fork; the C ABI needs a text contract it can embed | JSON Schema |
| Copy the provenance and scoring field contracts into this schema | one self-contained file | the two contracts drift the moment their sources change | pin the identity and reference the single source (HISS-19); the acceptance test checks the fixtures against the OpenAPI components |
| ADR-3311 append-only model (version 1 accepts unknown members, version 2 only for breaking changes) | fewer version bumps for pure additions | a reader silently accepts members it does not know; the event vocabulary is an enum, so a new event type is refused by validation anyway — the file would claim an openness it does not have | the closed model keeps the refusal exact and the byte-equal write-read-write identity check meaningful |
| `scoring_configuration` required | the schema always records the scoring options | a CLI-run timeline has no `ScoreOptions` surface to record; the skeleton's boundary fixture would fail | optional, with the OpenAPI-bound contract test |
| Spell non-finite values as the strings `NaN`/`+Inf`/`-Inf` | preserves the distinction in-document | the CLI writes `null`; a converter would translate, and a non-finite frame score could not be written at all | `null`, as the CLI writes it |

## Consequences

- **Positive**: a version-1 reader's refusal is exact (one named error at the
  offending member); the acceptance fixtures can pin one expected error list;
  the later byte-equal write-read-write check holds against a frozen shape.
- **Negative**: every added member or event type ships a new schema version,
  and readers refuse newer documents until they learn the version — visible
  breakage instead of silent ignorance.
- **Neutral / follow-ups**: the schema description and the docs page now state
  the closed model and point at this ADR; the C ABI writer/reader PRs reuse
  this versioning rule for their own shape pinning.

## References

- [#2452](https://github.com/VMAFx/vmafx/issues/2452) — the run-result timeline issue.
- [ADR-3311](3311-stream-metadata-field-list.md) — the append-only precedent.
- `docs/hardware-reports/report.schema.json` — the closed-object precedent.
- `api/openapi/vmafx-server-v1.yaml` — the single sources this schema references.
