# Research-0664: AI Model Sidecar Run Provenance

## Question

The modernization audit identified the AI `train_*.py` family as a high-value
duplication cluster. After ADR-0661 added shared `run_provenance` helpers for
MOS-head sidecars, should the same shape be widened to the FR regressor
trainers and tiny-VMAF exporters before the next model refresh wave?

## Findings

- `train_fr_regressor.py`, `train_fr_regressor_v2.py`, and
  `train_fr_regressor_v3.py` all write model sidecars, but the exact command,
  input table, and output targets were not recorded in a shared schema.
- v1 and v2 also write metrics JSON during failed or `--no-export` runs. Those
  files are the only durable artifact when a gate fails, so they need the same
  provenance block as the sidecar.
- `export_vmaf_tiny_v2.py`, `export_vmaf_tiny_v3.py`, and
  `export_vmaf_tiny_v4.py` turn local checkpoints into shipped ONNX/sidecar
  pairs, but the sidecar did not say which checkpoint or export invocation
  produced the artifact.
- The existing ADR-0661 helper already covers the required shape: entrypoint,
  argv, parsed args, named inputs, named outputs, and path hashes where files
  exist. A second helper would only recreate drift.

## Decision Pressure

K150K, CHUG, refreshed Netflix, and Phase-A tables are all being regenerated.
Without run provenance on FR regressor outputs, it is too easy to compare a new
checkpoint against a stale table or to promote a sidecar without knowing which
local corpus produced it.

## Chosen Follow-Up

Widen ADR-0661 adoption to the next model-producing script family:

- v1: sidecar and `--metrics-out` JSON include `run_provenance`.
- v2: sidecar and `--metrics-out` JSON include `run_provenance`.
- v3: sidecar includes `run_provenance`; smoke runs point at the generated
  temporary corpus so smoke artifacts remain visibly pipeline-only.
- `vmaf_tiny_v2` / `v3` / `v4` exporters: sidecars include
  `run_provenance` with the checkpoint input and ONNX/sidecar output targets.

This is a provenance-only change. It does not retrain weights, change feature
columns, modify registry semantics, or alter Netflix golden assertions.

## Validation

- Unit tests assert v2/v3 sidecar writers preserve the run-provenance block.
- Unit tests assert the vmaf_tiny v2/v3/v4 exporter sidecars preserve the same
  run-provenance block.
- Existing FR trainer tests cover corpus loading, LOSO schema, and ONNX smoke
  export paths.
