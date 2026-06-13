- **External-competitor benchmark harness — `tools/external-bench/`.**
  Side-by-side numerical comparison between the fork's
  `fr_regressor_v2_ensemble_v1` + `nr_metric_v1` predictors and two
  external OSS competitors (Synamedia/Quortex `x264-pVMAF`, GPL-2.0;
  DOVER-Mobile, Apache-2.0 + CC-BY-NC-SA 4.0). Wrapper-only
  architecture per [ADR-0368](../docs/adr/0368-external-bench-wrapper-only.md):
  each competitor lives in its own `<competitor>/run.sh` that invokes
  a user-installed external binary (path via env var) and re-shapes
  its output into a normalised JSON schema; no GPL'd code is
  vendored, linked, or copied into the fork. Ships
  `tools/external-bench/compare.py` orchestrator (BVI-DVC test fold
  + Netflix Public Drop corpus discovery, PLCC/SROCC/RMSE/runtime
  aggregation, fixed-width comparison-table renderer), four
  `run.sh` wrappers, 7 stubbed pytest cases that monkeypatch
  `subprocess.run` so tests never depend on external binaries
  being installed, and operator-facing
  [`README.md`](../tools/external-bench/README.md) documenting the
  licence boundary explicitly.
