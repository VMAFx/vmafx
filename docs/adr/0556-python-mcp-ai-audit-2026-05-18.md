<!-- markdownlint-disable MD013 MD060 -->
# ADR-0556: Python / MCP / AI silent-fallback audit fixes (2026-05-18)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `python`, `mcp`, `ai`, `vmaf-tune`, `correctness`, `audit`

## Context

A systematic read-pass across the Python / MCP / AI surfaces of the fork identified five
immediate-fix findings (two P0 silent correctness bugs, one P1 partially-plumbed backend
validation path, two P2/P3 surface defects) and five deferred T-rows. The two P0s produce
silent wrong-answer or misleading-success output in operator-facing workflows; the P2 causes
an operator to silently receive documented-placeholder encoder recipes instead of calibrated
ones without any visible indication.

The prior C-surface audit (2026-05-15) had already confirmed that the MCP backend enum gaps
(`vulkan`, `hip`, `metal` missing from `vmaf_score`/`describe_worst_frames`) were resolved.
This audit extends the same pattern check to the Python and AI scripts layers.

## Decision

We will fix the five immediate findings in one bundled DRAFT PR:

1. **P0**: `score.py` - wrap `json.load` in `try/except json.JSONDecodeError`; set `rc=65`,
   `payload=None`, skip parse paths. Corpus run continues with a NaN row instead of crashing.
2. **P0**: `bvi_dvc_to_full_features.py` - add early-return 2 with an actionable error message
   in both `_run_dir_mode` and `_run_zip_mode` when `entries` is empty. Prevents silent
   zero-row parquet with `exit(0)`.
3. **P2**: `auto.py` - promote `_LOG.debug` to `_LOG.warning` for the F.4 placeholder fallback
   in `_load_calibrated_recipes()`. Adds an actionable hint pointing at `--calibrate`.
4. **P3**: `server.py` - update `list_backends` tool description to enumerate all six backends
   (`cpu / cuda / sycl / vulkan / hip / metal`).
5. **P3**: `validate_model_registry.py` - replace silent `n=0` fallback with an `rc=1` error
   message when the post-validation count-read fails.

The P1 (compare/per-shot `select_backend()` pre-check gap) and three P3s (hardcoded path in
`permutation_importance.py`, `train_test_model.py` std=0 FIXME, `routine.py` swallowed
exception) are queued as T-rows in `docs/state.md` because each requires either a broader
refactor or a separate test pass to validate safely.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix all findings including P1 in a single PR | One review cycle | P1 touches 3 different subcommand paths in a 4094-line file; higher regression risk | Deferred; fixing P0/P2/P3 independently reduces blast radius |
| Raise `RuntimeError` on corrupt JSON in score.py instead of NaN row | Clearly surfaces the error | Crashes the whole corpus run for one bad score; operator loses all prior work | NaN row + non-zero exit allows the run to continue and flag the failure |
| Use `_LOG.info` instead of `_LOG.warning` for placeholder recipes | Less noisy | Default logging level is WARNING; INFO would still be invisible | `_LOG.warning` is the lowest level guaranteed visible at defaults |

## Consequences

- **Positive**: corpus runs survive a single vmaf process killed mid-write; zip/dir-mode AI
  scripts fail fast with actionable messages on mis-configured inputs; `vmaf-tune auto`
  operators are informed when their session uses placeholder recipes; MCP `list_backends` docs
  are accurate.
- **Negative**: `vmaf-tune auto` on a freshly set-up machine now prints a warning that was
  previously silent - this is the intended behaviour, but operators must supply a recipes JSON
  or run `--calibrate` to suppress it.
- **Neutral / follow-ups**: P1 (`select_backend()` pre-check in compare/per-shot paths) queued
  as T-PYTHON-COMPARE-NO-BACKEND-PRECHECK; see `docs/state.md`.

## References

- Research digest: [docs/research/python-mcp-ai-audit-2026-05-18.md](../research/python-mcp-ai-audit-2026-05-18.md)
- Prior MCP audit: `.workingdir/audit-2026-05-15/D-mcp-and-backends.md`
- ADR-0543: exit-code-100 enforcement for C-binary backend failures
- ADR-0498: strict-mode backend selection
- req: user requested exhaustive Python / MCP / AI audit matching the C-surface audit
