<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1120: Re-pin the vendored Pelorus interop ABI to minor-3 and consume PEL_SEC_COMPLEXITY in perceptual weighting

- **Status**: Accepted
- **Date**: 2026-06-27
- **Deciders**: Lusoris
- **Tags**: interop, abi, vendoring, pelorus, scoring, pooling, golden-gate, build, fork-local

## Context

The data-plane interop ABI is single-sourced in `VMAFx/pelorus` (ADR-0103) and
vmafx carries a pinned, read-only, append-only mirror of it (ADR-1113), pinned
at `pelorus@835e097` (ABI 1.0). Pelorus has since advanced to ABI minor-3
(`pelorus@818d844`), appending three sections — `PEL_SEC_QPREPORT` (1.1),
`PEL_SEC_MOTION_CONF` (1.2), and `PEL_SEC_COMPLEXITY` (1.3) — plus three new
source/header files (`denoise.h`, `denoise_params.c`, `qp_report_csv.c`). The
`PelorusSideData` header struct layout is unchanged (append-only, R1), and
vmafx's parser keys forward-compatibility on `abi_major` only, so this is a
deliberate re-pin, not a break. The CI drift-guard
(`scripts/sync-pelorus-interop.sh` without `--update`) fails on any divergence
from the pin, so the mirror must be re-vendored faithfully and the script's
manifest extended to cover the new files.

Separately, the perceptual spatial-pooling weighting (ADR-1118) currently
derives a per-frame salience from the banding-risk and variance sections only.
The new `PelorusComplexitySection` carries an aggregate per-frame complexity
scalar in `[0,1]`. Banding and compression artefacts are more perceptually
visible in low-complexity (flat, simple) content and masked in high-complexity
(busy, textured) content, so the salience boost should be attenuated as frame
complexity rises — a free perceptual-accuracy improvement the producer already
computes.

## Decision

We will (1) bump `PELORUS_VENDOR_SHA` to `818d844` and re-vendor the mirror to
ABI 1.3, adding `denoise.h`, `denoise_params.c`, and `qp_report_csv.c` to the
sync-script manifest (the last is required for the minor-3 conformance fixture
to link); (2) fix the `--update` mode so it also re-vendors the conformance
fixture body (it previously re-vendored only the six manifest files, leaving the
immediately-following drift check failing on a stale fixture); and (3) consume
`PEL_SEC_COMPLEXITY` in `perceptual_weight.c` by attenuating the derived
salience with a bounded factor `(1 - 0.5·complexity)`, floored at `0.25`. The
modulation is opt-in and golden-isolated: it engages only when perceptual
weighting is enabled AND a complexity section is present; an absent section
yields factor `1.0` (exact legacy behaviour), so the Netflix golden pairs —
which carry no side-data — score byte-identically (verified: mean VMAF
`76.667831` on the 576×324 golden pair is unchanged).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Re-vendor the full minor-3 surface (chosen) | Mirror stays a faithful, complete copy; fixture links; matches ADR-1113's append-only mirror contract | Adds two compiled TUs the fork does not yet otherwise call | Chosen — the fixture references `pel_x265_csv_parse`, and a partial mirror would diverge from the single source of truth |
| Trim the fixture / vendor only what links | Smallest diff | The fixture is the shared conformance contract; trimming it forks the test from pelorus and defeats the drift guard | Rejected — breaks the single-source-of-truth invariant |
| Attenuate salience by `(1 − k·complexity)` (chosen, k=0.5, floor 0.25) | Simple, bounded, monotone, documented; collapses to identity when the section is absent | One free constant to justify | Chosen — predictable and golden-safe |
| Gate the banding read on a complexity threshold (step function) | Cheaper conceptually | Discontinuous; a tiny complexity change flips weighting on/off | Rejected — non-monotone, harder to reason about and test |
| Replace banding salience with complexity outright | One signal | Throws away the per-cell spatial banding map ADR-1118 was built around | Rejected — complexity modulates, it does not replace |

## Consequences

- **Positive**: the mirror tracks pelorus ABI 1.3 with the drift guard green;
  perceptual weighting is more accurate (artefacts up-weighted on flat content,
  attenuated on busy content); the `--update` fixture-vendoring bug is fixed so
  future re-pins are a single `--update` + verify cycle.
- **Negative**: two new vendored TUs (`pelorus_denoise_params.c`,
  `pelorus_qp_report_csv.c`) compile into the library though only the latter is
  exercised by the fixture today; this is the cost of a faithful mirror.
- **Neutral / follow-ups**: the new vendored files inherit the existing
  lint/format/assertion-density exclusions automatically (prefix globs on
  `core/src/interop/pelorus_` and `core/include/libvmaf/pelorus/`); a
  load-bearing test (`test_complexity_modulates_weight`,
  `test_complexity_modulates_grid_zero`) pins the modulation and is
  toggle-proven (stubbing the consumer to a no-op fails the test).

## References

- ADR-0103 (single-source interop ABI), ADR-1113 (vendor the mirror),
  ADR-1118 (perceptual side-data weighting), ADR-0221 (changelog fragments),
  ADR-0165 (state.md bug tracking).
- Pelorus pin: `VMAFx/pelorus@818d844` (ABI 1.3).
- Source: `req` — per user direction to "re-vendor + consume the new section":
  re-pin the vendored ABI to minor-3 faithfully and consume
  `PEL_SEC_COMPLEXITY` to modulate the perceptual-weight strength.
