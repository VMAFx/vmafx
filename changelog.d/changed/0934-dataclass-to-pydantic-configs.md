- chore(ai): migrate user-input dataclass configs to pydantic v2 BaseModel
  (`vmaf_train.train.TrainConfig`, `vmaf_train.registry.ModelMetadata`,
  `vmaf_train.data.datasets.ManifestEntry`). The three classes are the only
  ones in `ai/src/vmaf_train/` parsed from operator-supplied YAML / JSON
  sidecars; everything else (report types, internal data carriers) stays as
  `@dataclass` deliberately. Replaces the hand-written
  `int(doc.get(...))` / `**doc` ingestion paths in `load_config()` and
  `registry.load()` with `model_validate()`, so bad inputs raise a
  ``ValidationError`` listing every offending field instead of crashing
  inside Python's keyword-argument machinery (`ModelMetadata(**doc)` was
  silently accepting unknown fields and refusing required ones with an
  un-actionable `TypeError`). Adds `pydantic>=2.13.4` to
  `ai/pyproject.toml` (already in tree via `mcp-server/vmaf-mcp`). Sidecar
  JSON layout is byte-identical (`ModelMetadata.to_json()` round-trips
  through `model_dump(mode="json")`). 667 ai tests still pass. See
  [ADR-0934](../docs/adr/0934-dataclass-to-pydantic-configs.md).
