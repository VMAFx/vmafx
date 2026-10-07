---
paths:
  - tools/vmaf-tune/src/vmaftune/cli.py
  - tools/vmaf-tune/tests/test_cli_subcommands.py
invariant: Output JSON is strict JSON; Pathlib-only filesystem ops; _stamp_tracked_default_sentinels tuple invariant.
---
<!-- markdownlint-disable MD024 -->
# CLI serialization and HISS-04 invariants

- **Phase-F executor result JSONL is strict JSON.** `run_plan`,
  `run_plan_per_shot`, and `run_plan_saliency` write
  `tune_results*.jsonl` through shared `vmaftune.jsonio`
  serialization path. Failed scores and all-failed per-shot
  weighted means stay `NaN` in memory for caller-side math, but
  serialize as `null` so strict JSONL consumers, report renderers,
  and FFmpeg profile readers never ingest JavaScript-only tokens.
- **All compare / report / benchmark / conformal / auto / ladder JSON output
  routes through `vmaftune.jsonio.dumps_strict` (ADR-0988 and restored
  BUG-048 contract).** Do not add bare `json.dumps` calls without NaN
  protection in `compare.py`, `report.py`, `benchmark.py`, `conformal.py`,
  `auto.py`, or `ladder.py` — import `dumps_strict` instead.
  Private `_nan_to_none` helpers in those modules were removed in
  ADR-0988; any reintroduction is rebase regression.

- **Help texts read code they describe.** `ladder --crf-sweep`
  prints `ladder.DEFAULT_SAMPLER_CRF_SWEEP`, `auto` help counts
  `auto.ShortCircuit`, and `corpus --two-pass` lists adapters with
  `supports_two_pass` (`_two_pass_encoders`). Do not write those values
  into help by hand again. Every `ADR-NNNN` in help string,
  `docs/usage/vmaf-tune*.md` page or AGENTS.d page must name record
  about vmaf-tune or one of cross-cutting records listed in
  `tests/test_help_texts_and_adr_refs.py`; many old vmaf-tune numbers now
  belong to other subsystems, so check title before citing. Pelorus's
  own records are written `Pelorus ADR-NNNN`.
- **`fast` names proxy's `unknown` slot.** production run whose
  encoder is outside `proxy.ENCODER_VOCAB_V2` writes stderr note and
  `proxy_encoder_slot` into JSON (`_fast_proxy_encoder_slot`); keep
  both when vocabulary or proxy call changes.

- **Fast-NR calibration sidecars are write-gated before tune
  consumes them.** `NRProxyBackend` intentionally trusts
  `calibration_slope`, `calibration_intercept`, and
  `calibration_threshold` once they are in `nr_metric_v1.json`;
  safety boundary is `ai/scripts/calibrate_nr_threshold.py`
  (ADR-0665), which refuses weak sample-count or PLCC fits by
  default. If fresh real-corpus run is rejected, fix NR
  model/features or training corpus; do not loosen vmaf-tune
  early-elimination logic to make bad sidecar useful.

- **Pathlib-only filesystem ops (ADR-0936).** Fork-owned modules
  under `tools/vmaf-tune/src/` and `tools/vmaf-tune/tests/` use
  `pathlib.Path` exclusively for filesystem operations — no
  `os.path.*`, `os.replace`, `os.symlink`, `os.path.getsize`,
  `os.path.splitext`, builtin `open()` on path argument, or
  `glob.glob`. ruff `PTH` ruleset (flake8-use-pathlib) is enabled
  in `pyproject.toml` and will fail lint on regression. When
  refactoring atomic-rename plumbing (e.g. `EncodeCache.put` blob
  commit), prefer `tmp_path.replace(dst)` over
  `os.replace(tmp_path, dst)` — they are semantically identical,
  pathlib-method form.

## _stamp_tracked_default_sentinels tuple invariant (ADR-1048)

Hard-coded tuple in `_stamp_tracked_default_sentinels` (cli.py
~line 147) must contain `dest=` value of every
`_TrackedDefaultAction` flag, not flag name. `ladder --duration`
uses `dest="duration_s"`, so both `"duration"` (corpus) and
`"duration_s"` (ladder) must be in tuple. When adding new
`_TrackedDefaultAction` flag with non-standard `dest=` argument,
add dest to tuple at same time or sentinel will never be set.

## HISS-04 helper-ordering invariants (tools-tune burn-down)

Splitting oversized functions into helpers moved three ordering
contracts out of single function bodies, where they were implicit,
into call sites. Each is silent when broken — no test name points at
it directly.

- **`_append_compare_rows` runs before `_append_sweep_rows`**
  (`report.py`, `build_encoder_profile`). Sweep pass reads
  `len(recommendations)` for its provisional `index`. Swapping calls
  renumbers rows before final sort overwrites them; result differs
  only if sort is ever made stable on `index`.

`_parity_probe_args` (`tests/test_fast_parity.py`) holds probe
parameters shared with Go twin `cmd/vmafx-tune`. Changing width,
height, framerate, preset or `sample_chunk_seconds` there without
matching twin breaks parity test, not either implementation.
