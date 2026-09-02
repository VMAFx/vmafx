<!-- markdownlint-disable MD013 MD060 -->
# Research digest: Python / MCP / AI stub-and-silent-fallback audit

**Date**: 2026-05-18
**ADR**: [ADR-0556](../adr/0556-python-mcp-ai-audit-2026-05-18.md)
**Status**: Accepted (P0/P1 fixes in PR; P2/P3 queued as T-rows in `docs/state.md`)

---

## Scope

Exhaustive read-pass across four surfaces for "stub, scaffold, dead wiring, silent fallback,
half-fix" patterns.

| Surface | Entry points audited |
|---|---|
| `tools/vmaf-tune/src/vmaftune/` | `score.py`, `auto.py`, `predictor_train.py`, `predictor.py`, `bisect.py`, `cli.py` (4094 lines) |
| `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` | All 7 MCP tools |
| `ai/scripts/` | `bvi_dvc_to_full_features.py`, `validate_model_registry.py` |
| `python/vmaf/` | `routine.py`, `core/train_test_model.py`, `core/local_explainer.py` |
| `scripts/dev/` | `permutation_importance.py` |

Prior MCP audit (2026-05-15, `.workingdir/audit-2026-05-15/D-mcp-and-backends.md`) confirmed
that the D.10 P0 findings (backend enum gaps, `_list_backends` returning 4 keys only) had
already been resolved. This audit extends to the Python harness and AI scripts.

---

## P0 - Silent correctness bugs (fixed in this PR)

**P0-SCORE-JSON-CORRUPT** `tools/vmaf-tune/src/vmaftune/score.py:368`
`json.load` had no `JSONDecodeError` guard. If `vmaf` exited 0 but wrote corrupt JSON
(killed mid-write), the exception propagated uncaught, crashing the entire corpus run.
Fix: wrap in `try/except json.JSONDecodeError`; set `rc=65`, `payload=None`, skip
`parse_vmaf_json` and `parse_feature_aggregates`; corpus row receives NaN score.

**P0-BVI-DIR-ZERO-CLIPS** `ai/scripts/bvi_dvc_to_full_features.py:545` (`_run_dir_mode`)
`_select_tier_entries_dir` returning an empty list caused the loop to silently iterate zero
times; `_write_parquet` wrote a zero-row parquet and `main()` returned 0. No warning visible
to the operator. Same issue in `_run_zip_mode` (line 493).
Fix: early-return 2 with a descriptive error message when `entries` is empty, in both modes.

---

## P1 - Feature accepted but not fully plumbed (queued as T-rows)

**P1-COMPARE-NO-BACKEND-PRECHECK** `tools/vmaf-tune/src/vmaftune/cli.py:2173,2729,2876`
Three code paths in `_build_per_shot_bisect_predicate`, `_run_compare` (bisect branch), and
`_run_compare_crf_sweep` (no-bisect branch) set `score_backend = None if arg == "auto" else arg`
and pass the raw string to `bisect_target_vmaf()` without calling `select_backend()` first.
If the user passes `--score-backend cuda` on a CPU-only binary, they get a cryptic vmaf binary
error mid-bisect instead of the friendly `BackendUnavailableError` exit 2 that corpus, ladder,
and fast subcommands produce. Tracked as **T-PYTHON-COMPARE-NO-BACKEND-PRECHECK** in state.md.

---

## P2 - Half-finished implementation (queued as T-rows)

**P2-AUTO-PLACEHOLDER-SILENT** `tools/vmaf-tune/src/vmaftune/auto.py:286`
`_load_calibrated_recipes()` logged the F.4 placeholder fallback at `DEBUG` level. The default
Python logging level is `WARNING`, so operators running `vmaf-tune auto` without an explicit
recipes JSON silently received documented-placeholder values (not measured outcomes per
Research-0067) with no visible indication.
Fix (in this PR): promoted to `_LOG.warning` with an actionable hint.

**P2-TRAIN-TEST-STD-ZERO** `python/vmaf/core/train_test_model.py:354`
`# FIXME: setting std to 0 may be misleading` - zero std default when `ys_label_stddev` is
absent from the stats dict. Can cause downstream division-by-zero or misleading uncertainty
estimates. Tracked as **T-PYTHON-TRAIN-TEST-STD-ZERO** in state.md.

**P2-ROUTINE-SWALLOWED-EXCEPTION** `python/vmaf/routine.py:604`
`except Exception as e: print("Warning: ..."); fallback to default stats` - swallows the
exception and continues with potentially incorrect stats. Tracked as
**T-PYTHON-ROUTINE-SWALLOWED-EXCEPTION** in state.md.

---

## P3 - Cleanup / cosmetic (queued as T-rows or fixed in this PR)

**P3-SERVER-LIST-BACKENDS-DESC** `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py:833`
`list_backends` tool description string listed `cpu / cuda / sycl / hip` - missing `vulkan`
and `metal`. Fix (in this PR): updated to `cpu / cuda / sycl / vulkan / hip / metal`.

**P3-PERMUTATION-IMPORTANCE-HARDCODED-PATH** `scripts/dev/permutation_importance.py:22`
`REPO = Path("/home/kilian/dev/vmaf")` - hardcoded developer-machine path; breaks on any
other host. Tracked as **T-PYTHON-PERMUTATION-IMPORTANCE-HARDCODED-PATH** in state.md.

**P3-VALIDATE-REGISTRY-MISLEADING-OK** `ai/scripts/validate_model_registry.py:183`
After successful jsonschema validation, a second `read_text` + `json.loads` to count entries
caught all exceptions and silently printed `"OK: 0 registry entries valid"` on any read
failure, creating a misleading success message. Fix (in this PR): propagate count-read failure
as `rc=1` with an `ERROR:` message.

**P3-LOCAL-EXPLAINER-HACKY** `python/vmaf/core/local_explainer.py:121`
`model = model[0]  # HACKY, TODO: fix it` - silently takes the first model from a list; may
produce wrong explanation if multiple models are present. Tracked as
**T-PYTHON-LOCAL-EXPLAINER-HACKY** in state.md.

---

## Confirmed non-issues (investigated and ruled out)

| Location | Finding | Verdict |
|---|---|---|
| `predictor_train.py:210` | Synthetic corpus rows use `schema_version: 2` vs current `SCHEMA_VERSION=3` | Not a bug - `_upgrade_row_in_place` fills missing v3 columns with NaN |
| `predictor.py:164` | Missing `onnxruntime` to analytical fallback | Intentional per docstring; not silent |
| `server.py:491` | `_compare_models` catches per-model exceptions into `errors` list | Intentional - caller sees all errors, not just first |
| `server.py:522` | `_load_vlm` `except Exception: continue` on VLM candidate scan | Intentional - VLM is best-effort |
| `server.py:699` | `finally: pass` in `_describe_worst_frames` | Correct - PNGs left for caller access until next invocation |
| `score_backend.py:205` | `select_backend()` raises `BackendUnavailableError` on explicit backend miss | Correct hard-fail; no silent fallback |
| `cli.py:3090` | `auto` subcommand `--execute`/`--runs-dir`/`--execute-all` | Fully consumed at lines 3090-3101 |
| `cli.py:1699` | `predict` subcommand all args | Fully consumed |
| `cli.py:3875` | Report JSON loading | Guarded with `(OSError, json.JSONDecodeError)` |

---

## References

- Prior MCP + backend audit: `.workingdir/audit-2026-05-15/D-mcp-and-backends.md`
- Research-0067: calibrated recipe provenance
- ADR-0543: exit-code-100 hard-fail for explicit GPU backend failures (C binary)
- ADR-0498: strict-mode enforcement for backend selection
- req: user requested exhaustive Python / MCP / AI audit matching the prior C-surface audit
