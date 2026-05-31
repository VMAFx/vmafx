<!-- markdownlint-disable MD013 MD060 -->
# ADR-0481: ADM p-norm Parameter Hardcoded at 3.0 — Deferral Decision

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: adm, predict, ai, testing

## Context

The ADM (Detail Loss Measure) extractor computes per-scale accumulation scores
with an L-p norm root step using a fixed exponent of `1/3.0`:

```c
float num_scale_h = powf(f_accum_h, 1.0f / 3.0f) + ...;
```

`AdmState` carries an `adm_p_norm` field (defaulting to `3.0f`) and an
`adm_p_norm` option declaration, but this field is **not wired** into
`adm_cm_partial_*` or the accumulation loop. A `//TODO` comment in
`integer_adm.c:2492` (pre-dating the fork) noted the parameterised form for
future integration.

The Python training harness (`quality_runner.py`) also hardcodes
`adm_p_norm=3.0f` during training and scoring. Changing this value produces
ADM scores outside the training distribution of the shipped `vmaf_v0.6.1`
family of models, violating the Netflix-golden gate.

## Decision

The `adm_p_norm` integration is formally deferred until a retrained model that
varies p-norm over the training corpus is produced and registered. The `//TODO`
comment is replaced with a `DEFERRED` block citing this ADR. The `adm_p_norm`
option on `AdmState` remains in the struct (it is part of the public option
schema and removing it would be an ABI break) but continues to have no effect
on computed scores.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wire `adm_p_norm` now | Enables p-norm tuning research | Breaks Netflix-golden gate; requires model retrain; no corpus currently supports it | Correctness risk outweighs benefit |
| Remove `adm_p_norm` option entirely | Reduces dead code | ABI break (field is in `VmafOption`); downstream code may set the option via dict and rely on the error-only path | Not justified yet |
| Leave as-is with //TODO | Avoids the decision | `//TODO` without a rationale is ambiguous; audits flag it as unresolved open work | Does not close the audit finding |

## Consequences

- **Positive**: the audit finding from `.workingdir/audit-todo-fixme-2026-05-16.md` item #6
  is closed; the deferred state is formally documented with a clear re-open trigger.
- **Negative**: `adm_p_norm` remains dead code until a retrained model ships.
- **Neutral / follow-ups**: if a future tiny-AI or retrained ADM model varies
  p-norm, the wiring in `adm_cm_partial_*` should be revisited in the same PR
  as the model registration, citing this ADR as the superseded deferral.

## References

- `core/src/feature/integer_adm.c` — DEFERRED comment at `adm_cm_partial_*` p-norm step
- TODO/FIXME audit: `.workingdir/audit-todo-fixme-2026-05-16.md` item #6
- `python/vmaf/core/quality_runner.py` — `adm_p_norm=3.0f` hardcoded in Python harness
