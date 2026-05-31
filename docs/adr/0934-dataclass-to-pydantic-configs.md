<!-- markdownlint-disable MD060 -->
# ADR-0934: Migrate user-input dataclass configs to pydantic v2 BaseModel

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: ai, validation, configs, modernization

## Context

`ai/src/vmaf_train/` ships ~21 `@dataclass` types. The two of them that
parse operator-supplied YAML (`TrainConfig`, `ManifestEntry`) and the one
that parses JSON sidecars on disk (`ModelMetadata`) used hand-written
ingestion paths:

```python
# Before — train.py
TrainConfig(
    model=doc["model"],                          # raises KeyError
    epochs=int(doc.get("epochs", 50)),           # raises TypeError if list
    val_frac=float(doc.get("val_frac", 0.1)),    # silently coerces "0.5"
    precision=doc.get("precision", "32-true"),   # type: ignore — unchecked
)

# Before — registry.py
ModelMetadata(**doc)                              # silently accepts extras
                                                  # raises un-actionable TypeError on missing
```

The fork ships `pydantic>=2.13.4` for `mcp-server/vmaf-mcp` already, so the
runtime cost of adopting it for these three classes is zero — only `ai/`
package metadata needs the extra `dependencies = [...]` entry.

The remaining 18 `@dataclass` types in `ai/src/vmaf_train/` are internal
result / report carriers (`NormReport`, `BisectResult`, `EvalReport`,
`CrossBackendReport`, `ModelAudit`, `LearnedFilterAuditReport`,
`ProfileResult`, `ProfileReport`, `QuantizationReport`,
`AllowlistReport`, `FrameStats`, `BackendComparison`, `FeatureDrift`,
`BisectStep`) or internal data carriers constructed in code, not parsed
from user input (`FrameSource`, `ScanEntry`, `Splits`, `Entry`). Migrating
them adds runtime validation overhead for fields that the producing code
already controls — net negative.

## Decision

Migrate only the three classes that ingest user-supplied YAML / JSON:

1. **`vmaf_train.train.TrainConfig`** — YAML config loaded via
   `load_config(path, overrides)`. Now `BaseModel(extra="forbid",
   validate_assignment=True)` with field validators rejecting
   `epochs <= 0`, `batch_size <= 0`, `val_frac >= 1`, `test_frac >= 1`,
   `seed < 0`.
2. **`vmaf_train.registry.ModelMetadata`** — JSON sidecar loaded via
   `registry.load(sidecar)`. Now `BaseModel(extra="forbid",
   validate_assignment=True)` with a `kind` field validator restricting to
   `VALID_KINDS`. `to_json()` round-trips via `model_dump(mode="json")` so
   the on-disk layout is byte-identical to the previous
   `asdict()` + `json.dumps(..., indent=2, sort_keys=True)`.
3. **`vmaf_train.data.datasets.ManifestEntry`** — YAML manifest row.
   `BaseModel(extra="forbid", frozen=True, validate_assignment=True)`
   with a `sha256` validator that rejects anything other than a 64-char
   lowercase hex digest at parse time.

All other `@dataclass` types stay untouched (KEEP triage).

Add `pydantic>=2.13.4` to `ai/pyproject.toml` (already in the venv via
`mcp-server/vmaf-mcp`'s pin).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Migrate every `@dataclass` in `ai/src/vmaf_train/` (~21 classes) | Uniform style across the package | Adds validation overhead on report types whose fields are already typed at production time; no actual user input path | Goal is line-numbered validation for *operator inputs*, not stylistic uniformity |
| Stay on `@dataclass` and add hand-rolled validation in `load_config()` / `registry.load()` | No new dep | Re-implements pydantic's error machinery; harder to extend with JSON-Schema export later | pydantic is already in the venv; rolling our own is anti-DRY |
| Migrate to `attrs` + `cattrs` | Lighter than pydantic | Adds a new dep absent from the venv; pydantic already in tree | New dep with no incremental capability |
| **Migrate only the three user-input classes (chosen)** | Validation where it matters; existing tests round-trip cleanly | Mixed `@dataclass` + `BaseModel` style within the package | Acceptable — the boundary is explicit (user-input vs producer-controlled) |

## Consequences

- **Positive**: Bad `train.yaml` files surface `ValidationError` listing
  every offending field with line / type context, instead of crashing
  inside `int(doc.get(...))` or `**doc` keyword splatting. ModelMetadata
  sidecars with stray fields (typos, schema drift) are caught at load
  time. The migrated classes can now export JSON-Schema via
  `Foo.model_json_schema()` if a downstream consumer needs it (no
  immediate caller, but the capability is free).
- **Negative**: Three of 21 dataclasses in `ai/src/vmaf_train/` are now
  `BaseModel`; the package mixes the two styles. Mitigated by clear
  triage rule (user input → BaseModel; everything else →
  `@dataclass`).
- **Neutral / follow-ups**: Sidecar JSON layout is byte-identical (golden
  test `test_register_roundtrip` passes). The `pydantic` dep is added to
  `ai/pyproject.toml` but was already resolvable in every venv that
  ships `vmaf-mcp`.

## References

- See [ADR-0042](0042-tinyai-docs-required-per-pr.md) for the tiny-AI
  docs-required rule that surfaces the user-input surfaces in scope.
- Source: req — `ai/src/vmaf_train/` MCP-tools audit (paraphrased):
  migrate user-input `@dataclass` configs to pydantic v2 BaseModel for
  declared validators, line-numbered errors, and JSON-Schema export.
