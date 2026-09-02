# Research 0679: DNN exporter manifest provenance

## Question

Which AI exporter sidecars still wrote durable model JSON without the shared
ADR-0661 `run_provenance` block?

## Findings

- `export_tiny_models.py` still wrote C2/C3 KoNViD sidecars directly with
  `json.dumps`, even though those sidecars are the registry-visible
  reproducibility boundary for `nr_metric_v1` and `learned_filter_v1`.
- `export_fastdvdnet_pre.py` and `export_transnet_v2.py` already pin upstream
  commits and checksums in their sidecars, but they did not record the local
  command, upstream input paths, or output targets used for a refresh.
- The placeholder variants have the same sidecar shape as the real exporters
  and should keep the same provenance contract for smoke-only rebuilds.
- The global `model/tiny/registry.json` remains an aggregate index; the
  per-model sidecar is the right place for per-run provenance.

## Decision

Add `run_provenance` to the DNN feature-model exporter sidecars, not to the
global registry. Keep the block optional at helper level so existing tests can
still construct minimal fixture sidecars when provenance is irrelevant.

## Commands

```bash
rg -n "json\\.dump|json\\.dumps|write_manifest_json|run_provenance" ai/scripts -g '*.py'
.venv/bin/python -m pytest ai/tests/test_dnn_exporter_run_provenance.py -q
```
