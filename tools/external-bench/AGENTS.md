# `tools/external-bench/` — agent notes

Parent: [../../AGENTS.md](../../AGENTS.md).

External-competitor benchmark harness. See
[ADR-0368](../../docs/adr/0368-external-bench-wrapper-only.md) for the
licence-boundary architecture,
[ADR-0656](../../docs/adr/0656-external-bench-wrapper-schema.md) for the
wrapper-output schema, and [`README.md`](README.md) for operator usage.

## Rebase-sensitive invariants

- **No GPL'd code in the fork.** The fork is BSD-3-Clause-Plus-Patent
  and `x264-pVMAF` is GPL-2.0. The harness MUST stay wrapper-only:
  every external competitor lives in `<competitor>/run.sh` and
  invokes a user-installed binary (path via env var). Never vendor,
  link, or copy code from any GPL'd competitor into this tree. If a
  reviewer flags "vendoring x264-pVMAF would be simpler" the answer
  is **no** — that would relicense the fork. ADR-0332 carries the
  reasoning.
- **Output schema is the contract between wrappers and `compare.py`.**
  Every `run.sh` MUST emit the JSON schema documented in
  `README.md` ("Schema" section): `frames[].{frame_idx,
  predicted_vmaf_or_mos, runtime_ms}` + `summary.{competitor, plcc,
  srocc, rmse, runtime_total_ms, params, gflops}`. Adding optional
  keys is fine; renaming or removing keys requires updating
  `compare.aggregate()` and every test in `tests/test_compare.py`
  in the same PR.
- **`summary.competitor` is the registry key, not a display label.**
  Wrapper payloads MUST use the exact key from `compare.WRAPPERS`
  (`fork-fr-regressor`, `fork-nr-metric`, `x264-pvmaf`,
  `dover-mobile`). Version/model detail belongs in optional metadata;
  otherwise `validate_wrapper_output()` rejects the payload before
  aggregation and the competitor silently drops from user reports.
- **Tests must not depend on external binaries.** Every test in
  `tests/test_compare.py` stubs `subprocess.run` so the suite runs
  green on any host. Do not add a test that requires `x264-pVMAF`
  or `dover-mobile` to be installed; if you need an integration
  test, gate it behind an opt-in `EXTERNAL_BENCH_INTEGRATION=1`
  env var and skip by default.
- **`run_wrapper` resolves `runner` at call time, not at definition
  time.** This is load-bearing for the `monkeypatch.setattr(
  compare.subprocess, "run", ...)` pattern in
  `test_main_emits_table_with_stubbed_wrappers`. Do not restore the
  default-arg binding `runner: SubprocessRunner = subprocess.run`.

## Adding a new competitor

1. Create `tools/external-bench/<competitor>/run.sh` mirroring the
   shape of an existing wrapper. Document the upstream URL and
   licence in the file header. Document the operator install steps
   in the same header.
2. Add `<competitor>` to the `WRAPPERS` dict in `compare.py`.
3. Add a row to the README's competitor table including the upstream
   licence.
4. If the competitor is GPL'd or otherwise copyleft, **call this out
   in ADR-0332's "Competitors covered" section** — the wrapper-only
   posture is what keeps the licence boundary clean.
5. Add a stubbed test under `tests/test_compare.py` exercising the
   new wrapper's schema-merge path.
