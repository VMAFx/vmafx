<!-- markdownlint-disable MD060 -->
# Research-0122 — `vmaf-tune sidecar` CLI wiring

## Context

ADR-0394 shipped the local sidecar model as a Python API: a
read-only predictor plus an additive online-ridge correction trained
from local encode residuals. The remaining scaffold gap was operator
access. Without a CLI, users had to write Python around
`SidecarPredictor` before the sidecar could absorb real encode
observations.

## Findings

- The existing sidecar model already has the required persistence and
  privacy contracts: random 128-bit host UUID, state under
  `<cache>/<predictor-version>/<codec>/state.json`, zero network path,
  and predictor-version invalidation.
- The CLI should therefore be a thin wrapper over the programmatic API,
  not a second training implementation.
- JSON and JSONL are enough for the first operator surface because
  `ShotFeatures` is already a stable named-field dataclass. Accepting
  both flat feature objects and `{ "features": ... }` wrappers lets the
  CLI consume hand-authored examples and future capture logs without a
  schema fork.
- The default predictor must stay the analytical fallback so the
  sidecar surface works on hosts without `onnxruntime`; `--model` is an
  opt-in for shipped predictor ONNX files.

## Decision Matrix

| Option | Pros | Cons | Decision |
|---|---|---|---|
| Thin argparse wrapper over `SidecarPredictor` | Reuses the tested model, cache layout, UUID posture, and predictor-version invalidation. Small surface: `status`, `predict`, `record`, `batch-record`. | Still requires callers to provide `ShotFeatures`; capture extraction is separate. | Chosen. This closes the operator access gap without expanding privacy scope. |
| Add automatic encode/log capture inside `sidecar record` | More end-to-end for operators. | Duplicates existing encode/score plumbing, pulls ffmpeg/vmaf process handling into the sidecar command, and risks hiding what features were recorded. | Rejected for this PR; capture extraction should be a later explicit workflow. |
| Upload captures to a shared pool | Long-term community learning path. | New consent, signing, transport, and aggregation policy surface. | Rejected; ADR-0394 deliberately keeps upload out of scope. |
| Train a non-linear tiny model from the CLI | Could model residual curvature. | Adds dependencies and a new model lifecycle; the current sidecar contract is zero-dep online ridge. | Rejected; requires separate ADR and corpus evidence. |

## Validation

Local command:

```bash
cd tools/vmaf-tune && ../../.venv/bin/python -m pytest tests/test_cli_sidecar.py tests/test_sidecar.py -q
```

This verifies the new CLI subparser, help surface, status JSON,
single-record persistence, prediction correction, and JSONL batch
recording, while preserving the existing sidecar model contracts.
