# `tools/external-bench/` — agent notes

Parent: [../../AGENTS.md](../../AGENTS.md).

External-competitor benchmark harness. See
[ADR-0368](../../docs/adr/0368-external-bench-wrapper-only.md) for the
licence-boundary architecture,
[ADR-0656](../../docs/adr/0656-external-bench-wrapper-schema.md) for the
wrapper-output schema, and [`README.md`](README.md) for operator usage.

## Rebase-sensitive invariants

- **No GPL'd code in fork.** `x264-pVMAF` = GPL-2.0, which ADR-0332
  judged incompatible with vendoring into this tree. Harness MUST stay
  wrapper-only: every external
  competitor lives in `<competitor>/run.sh`, invokes user-installed binary
  (path via env var). Never vendor, link, or copy code from any GPL'd
  competitor into this tree. Reviewer flags "vendoring x264-pVMAF would be
  simpler" -> answer = **no**, would relicense fork. ADR-0332 carries
  reasoning.
- **Output schema is contract between wrappers and `compare.py`.** Every
  `run.sh` MUST emit JSON schema documented in `README.md` ("Schema"
  section): `frames[].{frame_idx,
  predicted_vmaf_or_mos, runtime_ms}` + `summary.{competitor, plcc,
  srocc, rmse, runtime_total_ms, params, gflops}`. Adding optional
  keys fine; renaming or removing keys requires
  updating `compare.aggregate()` and every test in `tests/test_compare.py`
  in same PR.
- **`summary.competitor` = registry key, not display label.** Wrapper
  payloads MUST use exact key from `compare.WRAPPERS` (`fork-fr-regressor`,
  `fork-nr-metric`, `x264-pvmaf`, `dover-mobile`). Version/model detail
  belongs in optional metadata; otherwise `validate_wrapper_output()`
  rejects payload before aggregation, competitor silently drops from user
  reports.
- **Tests must not depend on external binaries.** Every test in
  `tests/test_compare.py` stubs `subprocess.run` -> suite runs green on any
  host. Do not add test requiring `x264-pVMAF` or `dover-mobile` installed;
  integration test -> gate behind opt-in `EXTERNAL_BENCH_INTEGRATION=1` env
  var, skip by default.
- **`run_wrapper` resolves `runner` at call time, not definition time.**
  Load-bearing for `monkeypatch.setattr(compare.subprocess, "run", ...)`
  pattern in `test_main_emits_table_with_stubbed_wrappers`. Do not restore
  default-arg binding `runner: SubprocessRunner = subprocess.run`.

## Adding a new competitor

1. Create `tools/external-bench/<competitor>/run.sh` mirroring shape of
   existing wrapper. Document upstream URL and licence in file header.
   Document operator install steps in same header.
2. Add `<competitor>` to `WRAPPERS` dict in `compare.py`.
3. Add row to README's competitor table including upstream licence.
4. Competitor GPL'd or otherwise copyleft -> **call out in ADR-0332's
   "Competitors covered" section** — wrapper-only posture keeps licence
   boundary clean.
5. Add stubbed test under `tests/test_compare.py` exercising new wrapper's
   schema-merge path.
