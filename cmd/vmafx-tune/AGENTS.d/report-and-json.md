---
paths:
  - pkg/report/*.go
  - cmd/vmafx-tune/cmd/report*.go
  - pkg/pyjson/**
invariant: Additive JSON schema; NaN -> null; Row vs WireRow split; report key probe; pyjson for parity.
---
<!-- markdownlint-disable MD013 MD029 -->
# Report schema, NaN coercion and CPython JSON parity

## Rebase-sensitive invariants

1. **JSON schema compatibility** (`pkg/report/report.go`): JSON output of
   `EmitJSON` must remain schema-compatible with Python `compare.py` v1/v2
   payloads. Python `report.py` renderer ingests this JSON directly. Any
   field rename or removal requires coordinated Python-side change. Add new
   optional fields only; never remove existing ones without schema-version bump.

2. **NaN coercion** (`pkg/report/report.go` `nanToNull`): `float64` fields
   that can be NaN (failed-row bitrate, VMAF, encode time) MUST serialize as
   JSON `null`, not bare `NaN` tokens. RFC 8259 strict parsers reject bare
   `NaN`. Mirror Python `_nan_to_none` discipline.

8. **`WireRow` vs `Row` duality** (`pkg/report/`): `Row` (report.go) =
   emit-only shape used by `compare` to produce JSON output; `WireRow`
   (multi.go) = unmarshal-only shape used by `report` to read that JSON back
   in. Intentionally separate types. Do not merge them — `Row` uses bare
   `float64` + NaN convention for emit; `WireRow` uses `*float64` for nullable
   unmarshal. Merging two would break either emit path (RFC 8259 bare NaN)
   or unmarshal path (null vs 0 ambiguity). ADR-0770.

9. **Schema auto-detection** (`cmd/vmafx-tune/cmd/report.go` `loadReportFile`):
   `report` subcommand probes `renditions` vs `rows` top-level keys to
   determine whether JSON file = ladder or compare payload. Probe =
   contract between two schemas. Any future schema omitting both keys
   must be added to probe before `report` subcommand can read it.

23. **Python-compatible JSON is not `encoding/json`** (`pkg/pyjson`, one
    CPython-JSON encoder since ADR-1137): every payload ported
    subcommand also emits from Python goes through `pyjson.Marshal` /
    `MarshalIndentSorted` / `MarshalStrict`, never
    `json.Marshal`/`MarshalIndent`. Go, CPython disagree on four:
    struct-field order vs sorted keys, HTML escaping, non-ASCII
    escaping, float rendering (`float64(92)` = `92` in Go, `92.0` in
    CPython; switch to exponent notation at 1e6 vs 1e16).
    `pyjson.FloatRepr` pinned against CPython `repr()` by
    `pkg/pyjson/testdata/float_repr.txt` corpus. Reaching for `encoding/json`
    in ported emit path silently breaks
    byte parity. (`compare`'s `emitSweepJSON` predates this package, still
    uses `MarshalIndent` with declaration-ordered struct fields; known
    gap, not pattern to copy.)
