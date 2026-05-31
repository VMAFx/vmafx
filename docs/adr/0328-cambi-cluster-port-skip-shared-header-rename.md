<!-- markdownlint-disable MD060 -->
# ADR-0328: Cambi cluster port — skip the shared-header rename

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: lusoris, agent-ad03c1b2db5286821 (Claude Opus 4.7)
- **Tags**: simd, port, cambi

## Context

Netflix upstream landed a 10-commit cambi cluster between 2026-05-06 and
2026-05-07 (`d655cefe` … `41bacc83`). The lusoris fork ports nine of these
commits verbatim. The tenth — `41bacc83` "feature/cambi: move shared code to
cambi.h" — relocates the shared inline helpers (`update_histogram_*`,
`uh_slide`, `uh_slide_edge`, `MAX`/`MIN`, `VmafRangeUpdater` typedef, the
`reciprocal_lut` LUT) and the `MAX/MIN` macros from `cambi.c` into a new
`core/src/feature/cambi.h` (renaming `cambi_reciprocal_lut.h` to it). It
also moves `calculate_c_values_avx2` from `cambi.c` into
`core/src/feature/x86/cambi_avx2.c`, since that function now needs the
shared helpers too.

The fork's port of `933cccb4` (frame-level calc_c_values dispatch) introduced
a `CAMBI_CALC_C_VALUES_BODY` macro inside `cambi.c` so the three driver
variants — scalar `calculate_c_values`, `calculate_c_values_avx2`, and a
fork-specific `calculate_c_values_neon` — share their loop nest verbatim
while binding to ISA-specific updaters at compile time. With the macro in
place, the helper inlines never need to be visible outside `cambi.c`, so
the upstream rename has no fork-side benefit.

The fork also has a separate `core/src/feature/cambi_internal.h` whose
purpose is ABI-stable shim functions for the Vulkan twin (ADR-0205 Strategy
II). Adopting upstream's `cambi.h` would create a second cambi public-ish
header file with overlapping responsibilities, which is exactly the kind of
collision the ADR-0028 maintenance rule warns against.

## Decision

We accept the cluster port as 9-of-10 commits and explicitly **skip
`41bacc83`**. The fork's `CAMBI_CALC_C_VALUES_BODY` macro stays in
`cambi.c`; `cambi_reciprocal_lut.h` is not renamed; no new `cambi.h` is
introduced. The fork-internal `cambi_internal.h` remains the only "shared
cambi inline" header.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A: Apply 41bacc83 verbatim** (rename `cambi_reciprocal_lut.h` → `cambi.h`, move helpers, move `calculate_c_values_avx2` into `cambi_avx2.c`, move a fork-added `calculate_c_values_neon` into `arm64/cambi_neon.c`) | Maximal rebase friendliness; identical layout to upstream. | Requires deleting the fork's `CAMBI_CALC_C_VALUES_BODY` macro (regression on dispatch DRYness for the 3-variant fork). Adds a second cambi header alongside `cambi_internal.h` — confusing co-existence. NEON `arm64/cambi_neon.c` would need to grow `calculate_c_values_neon`, expanding the fork-specific surface. | Drops a working DRY abstraction for marginal upstream-mirror gain. |
| **B: Skip 41bacc83 entirely** *(chosen)* | Preserves the macro DRY dispatch; no header collision; minimal diff churn. | Future cambi syncs that touch the moved helpers will need fork-side adaptation (same as today). | Documented as an intentional partial port with a tracking entry in `docs/rebase-notes.md`. |
| C: Apply only the rename (`cambi_reciprocal_lut.h` → `cambi.h`), keep helpers in `cambi.c` | File-name parity with upstream. | The point of the rename in 41bacc83 is to give the helpers a home — without that, the rename is pure churn. | Half-measure with no payoff. |

## Consequences

- **Positive**: cambi.c stays self-contained for the inline helpers; the
  macro-based 3-variant dispatch keeps the inc/dec/row triples in one place;
  no new cambi.h conflicts with `cambi_internal.h`.
- **Negative**: any future upstream cambi commit that depends on the
  cambi.h location (e.g. a future ISA shim including `cambi.h` to reuse
  `update_histogram_*`) will need fork-side translation to the macro. The
  rebase-notes entry tracks this.
- **Neutral / follow-ups**: tracked in
  [`docs/rebase-notes.md`](../rebase-notes.md) under
  `RN-2026-05-08-cambi-cluster`. If a future port introduces a need for
  cross-TU access to the helpers, we revisit by promoting the helpers to a
  fork-local `cambi_inlines.h` (separate from both `cambi_internal.h` and
  upstream's `cambi.h`).

## References

- Upstream commit `41bacc83e1995f05bbbe17c6ee549dc25ada46d5`
  ("feature/cambi: move shared code to cambi.h").
- Upstream cluster: `d655cefe` … `41bacc83` (10 commits, see
  [`docs/research/0089-upstream-sync-coverage-2026-05-08.md`](../research/0089-upstream-sync-coverage-2026-05-08.md)).
- Fork macro: `CAMBI_CALC_C_VALUES_BODY` in
  `core/src/feature/cambi.c` (introduced by the port of `933cccb4`).
- Fork shim header: `core/src/feature/cambi_internal.h` (ADR-0205
  Strategy II).
- ADR maintenance rule: [ADR-0028](0028-adr-maintenance-rule.md).
- Touched-file lint-clean rule: [ADR-0141](0141-touched-file-cleanup-rule.md).
- Source: `req` — task brief: "If a commit fundamentally can't be ported
  because the fork's cambi has diverged irreconcilably, STOP and report —
  don't fudge. Note the gap in the PR's Known follow-ups."
