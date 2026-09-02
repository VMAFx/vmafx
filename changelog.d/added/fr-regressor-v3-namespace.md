- **`fr_regressor_v3` namespace map + `fr_regressor_v3plus_features`
  reservation (ADR-0349).** Resolves the namespace collision agent
  reports `abd6ed552ac8cae60` and `abda108c8263491da` surfaced
  between the existing production `fr_regressor_v3` checkpoint
  (vocab-16 retrain shipped via ADR-0323 / PR #428,
  sha256 `eaa16d23…`, `smoke: false`) and a future "feature-set
  v3" workstream (canonical-6 + `encoder_internal` + shot-boundary
  + `hwcap`). The existing v3 production row stays bit-identical
  (zero file moves, zero sha256 churn — investigation found 19
  references in 12 files, all keep working unchanged); the future
  feature-set bump lands as `fr_regressor_v3plus_features`,
  reserved here in [ADR-0349](../docs/adr/0349-fr-regressor-v3-namespace.md)
  + [`ai/AGENTS.md`](../ai/AGENTS.md). The reservation is
  documentation-only because
  [`core/test/dnn/test_registry.sh`](../core/test/dnn/test_registry.sh)
  treats every registry row as a hard contract — a stub row would
  fail CI on day one, so the row lands with the future PR that
  ships the `.onnx`. Rejected: renaming the existing v3 to
  `_v3_vocab16` (touches 19 call sites; breaks ADR-0291
  production-flip immutability) and calling the future work
  `_v4_features` (inflates `_v4` to a name-conflict workaround).
