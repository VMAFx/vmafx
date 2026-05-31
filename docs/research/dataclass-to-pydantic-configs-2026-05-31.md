<!-- markdownlint-disable MD060 -->
# Research: User-input dataclass → pydantic v2 migration triage — 2026-05-31

- **Status**: Closed (implemented in ADR-0934)
- **Workstream**: AI training package modernization
- **Last updated**: 2026-05-31
- **Author**: Modernization sweep agent

## TL;DR

`ai/src/vmaf_train/` ships **21** `@dataclass` definitions across 16
modules. Triaging each by ingestion source:

- **3 are operator inputs** (parsed from YAML / JSON on disk):
  `TrainConfig` (`train.py`), `ModelMetadata` (`registry.py`),
  `ManifestEntry` (`data/datasets.py`). These three migrate to
  `pydantic.BaseModel(extra="forbid")` with field validators.
- **18 are producer-controlled** (constructed in code by `train.py`,
  `audit.py`, `eval.py`, `cross_backend.py`, `profile.py`,
  `quantize.py`, etc.). These stay as `@dataclass`. Migrating them
  would impose validation overhead on fields whose values the
  producing code already controls, with zero correctness upside.

Net: 3 of 21 (~14%) migrate. The boundary is the user-input edge,
nothing else.

## Triage table

| Class | Module | Source | Migration | Reason |
|---|---|---|---|---|
| `TrainConfig` | `train.py` | `yaml.safe_load` → `load_config()` | **MIGRATE** | User-supplied YAML; `int(doc.get(...))` ingestion |
| `ModelMetadata` | `registry.py` | `json.loads(sidecar)` → `load()` via `**doc` | **MIGRATE** | JSON sidecar; `**doc` splat silently accepts unknown fields |
| `ManifestEntry` | `data/datasets.py` | `yaml.safe_load` → `load_manifest()` | **MIGRATE** | YAML manifest row; sha256 / mos malformedness should surface at parse time |
| `FrameSource` | `data/frame_loader.py` | Constructed in scanner | KEEP | Internal data carrier |
| `FeatureDrift` | `validate_norm.py` | Produced by `validate()` | KEEP | Internal report row |
| `NormReport` | `validate_norm.py` | Produced by `validate()` | KEEP | Internal report root |
| `ScanEntry` | `data/manifest_scan.py` | Produced by `scan()` | KEEP | Producer-controlled |
| `BisectStep` | `bisect_model_quality.py` | Produced by `bisect()` | KEEP | Internal step record |
| `BisectResult` | `bisect_model_quality.py` | Produced by `bisect()` | KEEP | Internal report root |
| `ModelAudit` | `audit.py` | Produced by `audit_model()` | KEEP | Internal report |
| `FrameStats` | `learned_filter_audit.py` | Produced by audit | KEEP | Internal stat row |
| `LearnedFilterAuditReport` | `learned_filter_audit.py` | Produced by audit | KEEP | Internal report root |
| `Splits` | `data/splits.py` | Produced by `split_keys()` | KEEP | Deterministic compute output |
| `EvalReport` | `eval.py` | Produced by `correlations()` | KEEP | Producer-controlled |
| `BackendComparison` | `cross_backend.py` | Produced by `compare()` | KEEP | Internal stat row |
| `CrossBackendReport` | `cross_backend.py` | Produced by `compare()` | KEEP | Internal report root |
| `ProfileResult` | `profile.py` | Produced by `profile()` | KEEP | Internal stat row |
| `ProfileReport` | `profile.py` | Produced by `profile()` | KEEP | Internal report root |
| `Entry` | `data/feature_dump.py` | Constructed in `dump_features()` callers | KEEP | Producer-controlled |
| `AllowlistReport` | `op_allowlist.py` | Produced by `check()` | KEEP | Internal report |
| `QuantizationReport` | `quantize.py` | Produced by `quantize()` | KEEP | Internal report root |

## Why this matters

The pre-migration ingestion paths fail in user-hostile ways:

- `TrainConfig(model=doc["model"], ...)` — raises `KeyError: 'model'`
  with no file/line context if the YAML is missing `model:`.
- `ModelMetadata(**doc)` — `TypeError: __init__() got an unexpected
  keyword argument 'plcc_target'` if the sidecar contains a stray
  field; silently accepts arbitrary garbage if the field is mis-typed
  to match an existing argname.
- `int(doc.get("epochs", 50))` — raises `TypeError: int() argument
  must be a string ...` if the YAML value is a list.

Post-migration:

- `pydantic.ValidationError` lists every offending field with the
  expected type and the actual offending value.
- `extra="forbid"` makes mis-spelled keys a parse-time error.
- `field_validator`s enforce semantic ranges (`val_frac < 1`,
  `epochs > 0`, `sha256` 64-char hex) that previously crashed the
  trainer mid-run with a cryptic stacktrace.

## What did NOT migrate (and why)

The 18 internal report / data-carrier dataclasses stay as
`@dataclass`. Their fields are produced by code that has already
type-checked or computed them — adding a pydantic validator pass on
construction adds CPU work without catching anything mypy / unit
tests don't catch first. Mixing styles inside the package is
acceptable because the boundary is principled: anything that can
parse a string-from-disk uses pydantic; anything that consumes a
typed value from a typed function uses dataclass.

## Compatibility

- `ModelMetadata.to_json()` was reimplemented via
  `model_dump(mode="json")` + the same `json.dumps(indent=2,
  sort_keys=True)` tail to keep the on-disk sidecar layout
  byte-identical. The pre-existing `test_register_roundtrip` golden
  test passes unchanged.
- `pydantic>=2.13.4` was added to `ai/pyproject.toml`. The container
  venv already pins this version via `mcp-server/vmaf-mcp`'s
  dependency tree, so no environment rebuild was required.

## References

- ADR-0934 (this work)
- mcp-server/vmaf-mcp pydantic pin (`>=2.13.4`)
