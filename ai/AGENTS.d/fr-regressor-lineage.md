---
paths:
  - ai/src/vmaf_train/models/fr_regressor.py
  - docs/ai/models/fr_regressor*.md
invariant: fr_regressor lineage versioned by encoder vocab (_vN) and feature set (_vNplus_features); names reserved.
---
<!-- markdownlint-disable MD013 MD060 -->
# `fr_regressor_*` namespace map (ADR-0349)

`fr_regressor` lineage carries two orthogonal axes. Encoder-vocab
versioning runs on `_v{N}` (v1 = no codec block, v2 = 13-slot, v3 = 16-slot).
Feature-set versioning runs as `_v{N}plus_features` suffix on matching
encoder-vocab base. Names below are claimed; do **not** reuse them for
unrelated workstreams.

| Name | Encoder vocab | Feature axis | Status |
|---|---|---|---|
| `fr_regressor_v1` | none (single-input) | canonical-6 | shipped (ADR-0249) |
| `fr_regressor_v2` | v2 (13-slot) | canonical-6 + 8-D codec block | shipped (ADR-0272 / ADR-0291) |
| `fr_regressor_v2_ensemble_v1_seed{0..4}` | v2 (13-slot) | canonical-6 + 8-D codec block | shipped (ADR-0393) |
| `fr_regressor_v3` | v3 (16-slot) | canonical-6 + 18-D codec block | shipped (ADR-0302 / ADR-0323) |
| `fr_regressor_v3plus_features` | v3 (16-slot) | canonical-6 + `encoder_internal` + shot-boundary + `hwcap` | **reserved** (ADR-0349) — registry row lands with future PR that ships `.onnx` |

Reservation is documentation-only because
[`core/test/dnn/test_registry.sh`](../../core/test/dnn/test_registry.sh)
treats every registry row as hard contract (file must exist, sha256 must
match, sidecar must accompany every `smoke: false` entry); stub row would
fail CI on day one. Future `_v3plus_features` PR populates row in
same commit that ships `.onnx`. See
[ADR-0349](../../docs/adr/0349-fr-regressor-v3-namespace.md) for namespace
decision and rejected alternatives.
