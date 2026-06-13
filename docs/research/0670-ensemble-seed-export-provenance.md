<!-- markdownlint-disable MD060 -->
# Research-0670: Ensemble Seed Export Provenance

## Summary

The shared ADR-0661 provenance work covered FR-regressor trainers and tiny-VMAF
exporters, but `ai/scripts/export_ensemble_v2_seeds.py` still wrote the
production ensemble seed sidecars with a direct `json.dumps()` call and no
`run_provenance` block.

That exporter is a promotion boundary: it consumes a full-corpus table and a
`PROMOTE.json` LOSO verdict, trains final per-seed ONNX members, and writes the
sidecars that model loading and registry validation depend on. Fresh seed
sidecars need to preserve the corpus/verdict/argv lineage that justified the
export.

## Files Audited

- `ai/scripts/export_ensemble_v2_seeds.py`
- `docs/ai/models/fr_regressor_v2_probabilistic.md`
- `docs/ai/ensemble-training-kit.md`
- ADR-0661 and `aiutils.run_manifest`

## Findings

- Per-seed sidecars already contain training recipe, corpus hash, ONNX hash,
  and gate evidence, but not the original command or parsed export arguments.
- Registry patching is optional and should be recorded as an output target when
  `--update-registry` is used.
- The existing `aiutils.run_manifest` helper fits the exporter without a new
  schema.

## Decision Matrix

| Option | Pros | Cons | Result |
|---|---|---|---|
| Keep legacy direct sidecar JSON | Smallest diff | Fresh seed exports remain less traceable than other model sidecars | Rejected |
| Add a bespoke `export` object | Localized fields | Duplicates ADR-0661 path and argv normalization | Rejected |
| Attach ADR-0661 `run_provenance` to each per-seed sidecar | Shared schema; records corpus, PROMOTE verdict, argv, and output targets | Slightly larger sidecars | Chosen |

## Outcome

`export_ensemble_v2_seeds.py` builds one shared `run_provenance` block per
export invocation and attaches it to every seed sidecar. Sidecar and optional
registry writes use `write_manifest_json()` for deterministic JSON formatting.

## Validation

```bash
.venv/bin/ruff check \
  ai/scripts/export_ensemble_v2_seeds.py \
  ai/tests/test_export_ensemble_v2_seeds_provenance.py

.venv/bin/python -m pytest ai/tests/test_export_ensemble_v2_seeds_provenance.py -q
```
