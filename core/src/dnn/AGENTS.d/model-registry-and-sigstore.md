---
paths:
  - model/tiny/registry.json
  - model/tiny/registry.schema.json
  - model/tiny/*.json
  - core/src/dnn/model_loader.c
  - ai/scripts/validate_model_registry.py
  - ai/src/aiutils/onnx_signature.py
invariant: Model registry entries must validate against schema and verify Sigstore bundles with cosign via posix_spawnp.
---
<!-- markdownlint-disable MD013 -->
# Model Registry Schema and Sigstore Verification

- **Registry schema is trust contract** (T6-9 / [ADR-0211](../../../../docs/adr/0211-model-registry-sigstore.md)).
  Every entry in [`model/tiny/registry.json`](../../../../model/tiny/registry.json)
  must satisfy [`registry.schema.json`](../../../../model/tiny/registry.schema.json):
  required `id` / `kind` / `onnx` / `sha256`, plus `license` and
  `sigstore_bundle` for `schema_version: 1` entries. New fields
  added by extending schema first, then registry, then any
  consumers — never other way around. `--tiny-model-verify`
  path in `model_loader.c` parses registry inline (no JSON dep),
  spawns `cosign` via `posix_spawnp(3p)`; `system(3)` is and stays
  banned.

- **Model registry + Sigstore (T6-9, PR #199 open, ADR-0211
  placeholder)**: `--tiny-model-verify` flag wires through to
  `cosign verify-blob` against Sigstore bundle declared in
  registry. Pairs with `quant_mode` / `int8_sha256`
  fields from
  [ADR-0173](../../../../docs/adr/0173-ptq-int8-audit-impl.md) /
  [ADR-0174](../../../../docs/adr/0174-first-model-quantisation.md).
  On merge: every shipped tiny-AI model needs Sigstore bundle
  path in `model/tiny/registry.json`.

- **Metadata matches graph ([ADR-1546](../../../../docs/adr/1546-tiny-model-metadata-against-graphs.md))** —
  `ai/scripts/validate_model_registry.py` (required check "Tiny-Model
  Registry Validate") reads every registered graph and its int8 sibling
  with `ai/src/aiutils/onnx_signature.py` (no `onnx` package); fails
  when registry `opset` or sidecar's `opset`, `sha256`,
  `input_names` / `output_names`, `input_name` / `output_name`,
  feature-list length, `encoder_vocab` (width + 2), `codec_block_dim` or
  `codec_block_layout` disagrees with graph. re-export updates
  sidecar and registry row with graph; exporters record opset
  file imports (torch raises requested 17 to 18). runtime does
  not check registry digests: only `--tiny-model-verify` reads
  registry, for bundle path.
