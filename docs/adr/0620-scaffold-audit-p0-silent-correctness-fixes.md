<!-- markdownlint-disable MD060 -->
# ADR-0620: Scaffold audit P0 — three silent-correctness fixes

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `python`, `correctness`, `bugfix`, `fork-local`

## Context

The 2026-05-19 scaffold audit (`docs/research/scaffold-audit-2026-05-19.md`)
identified three P0 silent-correctness bugs in the Python harness — all tracked
in `docs/state.md` under `T-PYTHON-ROUTINE-SWALLOWED-EXCEPTION`,
`T-PYTHON-TRAIN-TEST-STD-ZERO`, and `T-PYTHON-LOCAL-EXPLAINER-HACKY`.

Each bug produces wrong output without any exception reaching the caller:

1. **`routine.py:604`** — `except Exception: print/fallback` swallowed any
   failure during the extended-stats calculation path and silently continued
   with uncalibrated normalisation stats, producing misleadingly narrow
   confidence intervals and silently wrong PLCC/SROCC when the training
   distribution was non-standard.

2. **`train_test_model.py:354`** — `plot_scatter` substituted
   `np.zeros(len(ys_label))` for `ys_label_stddev` when the key was absent
   from the stats dict. Downstream callers (error-bar rendering, `evaluate_stddev`)
   used the zero array as a normalisation factor, producing incorrect
   visualisations.

3. **`local_explainer.py:121`** — `model = model[0]  # HACKY, TODO` silently
   took only the first model from an ensemble list. Callers passing a
   multi-model bootstrap ensemble obtained per-feature importance numbers
   from seed-0 only, with no diagnostic.

All three violate the SEI CERT C / CERT Python rule that error paths must be
explicit and diagnosable. The bugs had been tolerated because the fallback path
yielded plausible (but wrong) output — the hardest failure mode to detect.

## Decision

Replace each silent-fallback with an explicit raise:

1. **`routine.py`**: replace the bare `except Exception: print/fallback` with
   `raise CalibrationError(...) from exc` when `allow_uncalibrated=False`
   (the new default-safe parameter on `run_test_on_dataset`). Callers that
   genuinely want the uncalibrated fallback pass `allow_uncalibrated=True`.

2. **`train_test_model.py`**: replace `np.zeros` substitution with
   `raise MissingLabelStddevError(...)`. Callers that intentionally want
   unit error bars pass `assume_unit_stddev=True` to `plot_scatter`.

3. **`local_explainer.py`**: replace the silent `model[0]` pick with
   `raise EnsembleNotSupportedError(...)` for `len(model) > 1`. A
   single-element list continues to work (unwraps transparently).

All three exception classes are added to `python/vmaf/tools/exceptions.py`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the warning-and-fallback, just improve the warning message | No caller breakage; zero migration cost | Silent wrong output persists; callers cannot distinguish warning from success | The whole problem is silent wrong output; a louder warning does not fix that |
| Raise unconditionally (no opt-in flag) | Strictest posture | Breaks existing callers that relied on the fallback intentionally | `allow_uncalibrated` / `assume_unit_stddev` carry zero cognitive overhead and preserve backward compat for deliberate callers |
| Iterate the ensemble and average explanations | Fixes P0-3 without raising | Semantics are undefined (weighted? unweighted? which seed?) — ship correctness first, aggregation strategy in a follow-on | Semantically ambiguous; a well-typed exception unblocks the caller to make an explicit choice |

## Consequences

- **Positive**: callers that hit these code paths now get a diagnosable
  exception with a message pointing at the opt-in flag; silent wrong output
  is eliminated.
- **Negative**: any caller that relied on the silent fallback without
  `allow_uncalibrated=True` or `assume_unit_stddev=True` will now raise.
  The migration is a one-liner per call site.
- **Neutral / follow-ups**:
  - P0-3 follow-up: implement ensemble aggregation (averaged feature weights)
    once the semantics are agreed; at that point `EnsembleNotSupportedError`
    becomes optional.
  - `T-PYTHON-ROUTINE-SWALLOWED-EXCEPTION`, `T-PYTHON-TRAIN-TEST-STD-ZERO`,
    and `T-PYTHON-LOCAL-EXPLAINER-HACKY` closed in `docs/state.md`.

## References

- `docs/research/scaffold-audit-2026-05-19.md` §P0-1, §P0-2, §P0-3
- `docs/adr/0556-python-mcp-ai-audit-2026-05-18.md` (original audit ADR that
  opened the three state.md tracking rows)
- `python/vmaf/tools/exceptions.py` (new exception classes)
- `python/vmaf/routine.py` (P0-1 fix)
- `python/vmaf/core/train_test_model.py` (P0-2 fix)
- `python/vmaf/core/local_explainer.py` (P0-3 fix)
- `python/test/test_adr0620_scaffold_audit_p0.py` (16 regression tests)
