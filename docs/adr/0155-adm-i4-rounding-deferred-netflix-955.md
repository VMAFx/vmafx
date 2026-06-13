# ADR-0155: Defer fix for Netflix#955 — `i4_adm_cm` int32 rounding overflow

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: correctness, adm, netflix-upstream, deferred, golden-gate

## Context

Netflix upstream issue
[#955](https://github.com/Netflix/vmaf/issues/955) ("Weird rounding
in i4_adm_cm") reports an integer-overflow bug in ADM's scale-1/2/3
"integer 4" code path:

```c
/* core/src/feature/integer_adm.c (scale loop) */
const uint32_t shift_flt[3] = {32, 32, 32};
int32_t add_bef_shift_dst[3], add_bef_shift_flt[3];

for (unsigned idx = 0; idx < 3; ++idx) {
    add_bef_shift_dst[idx] = (1u << (shift_dst[idx] - 1));
    add_bef_shift_flt[idx] = (1u << (shift_flt[idx] - 1));  /* ← bug */
}
```

For `shift_flt[idx] == 32`, the expression `1u << 31` yields
`0x80000000` (unsigned). Assigning it into `int32_t` wraps to
`-2147483648`. The intended rounding term — half of
`1 << 32` = `2^31` — becomes **negated**. Downstream, every
`I4_ADM_CM_THRESH_S_I_J`-style expansion does

```c
sum += (int32_t)((((int64_t)I4_ONE_BY_15 * abs(val)) + add_bef_shift) >> shift);
```

so scales 1–3 perform `(product - 2^31) >> 32` instead of
`(product + 2^31) >> 32`. The numerical effect: each summed term
is too small by ≈1 LSB, biasing all three ADM scales low.

The bug exists verbatim in the fork. Fork PR #44
("feat(libvmaf/feature): port upstream ADM updates, Netflix
`966be8d5`") ported 8 ADM files wholesale via `git checkout
966be8d5 -- …` and preserved the overflow. The Netflix golden
assertions (`python/test/quality_runner_test.py`,
`vmafexec_test.py`, `feature_extractor_test.py`) — which
[ADR-0024](0024-netflix-golden-preserved.md) froze as the
numerical ground truth — encode the *buggy* ADM outputs. Our
current VMAF-mean on the golden `src01_hrc00/01_576x324` pair is
`76.66890…`, within `places=4` of Netflix's published number, and
that number bakes in the `-2^31` rounding.

Upstream Netflix#955 has been **OPEN since 2020** with no
maintainer response. The fork has no upstream authority to track
and no user-facing bug report against the fork.

## Decision

**Leave the bug in place.** We document the inherited overflow
(in this ADR, the rebase-notes ledger, the in-file code comment,
and `AGENTS.md`), but we do not patch `integer_adm.c` and we do
not add a feature flag that would change ADM output. The Netflix
golden gate (project hard rule #1) is the stronger constraint
here: shifting ADM scores to "mathematically correct" without a
coordinated Netflix-side assertion update would silently diverge
from the published VMAF numbers that every downstream integrator
has calibrated against.

The backlog item T1-8 is closed as **"verified present,
deliberately preserved for Netflix-golden bit-exactness"**. If
Netflix ever lands a fix upstream (thereby moving the golden
numbers), the fork will sync on `/sync-upstream` per
[ADR-0142](0142-port-netflix-18e8f1c5-vif-sigma-nsq.md)'s
Netflix-authority precedent — re-run `make
test-netflix-golden` after the rebase and adopt any new
expectations upstream ships.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Leave the bug (this ADR)** | Preserves Netflix-golden bit-exactness; matches published VMAF numbers; zero risk to downstream calibrations | ADM scales 1–3 remain biased low by ≈1 LSB | **Chosen** — matches hard rule #1 until Netflix moves |
| **Opt-in `fix_adm_rounding` feature-dict flag** | Caller who wants correct math can opt in; default preserves golden | Doubles the ADM numerical contract (two sets of expected outputs to maintain); encourages drift; no known downstream caller actually asking for this | Rejected — no user demand; maintenance cost ≫ value |
| **Fix + update Netflix golden** per [ADR-0142](0142-port-netflix-18e8f1c5-vif-sigma-nsq.md) carve-out | Mathematically correct; simpler code | Netflix hasn't moved on #955 in ~6 years; the carve-out is "upstream-authored test update the fork tracks" — there is no upstream authority to track here. Violates hard rule #1 without cover. | Rejected — premature; no upstream precedent to cite |
| **Widen `add_bef_shift_flt[]` to `uint32_t` unconditionally** | Minimal diff; correct math | Same Netflix-golden break as the opt-in fix but without even a flag to disable it | Rejected — same reason, plus it's a silent visible-behaviour change |

## Consequences

- **Positive**:
  - Netflix-golden assertions stay pass/green with zero drift,
    satisfying project hard rule #1 and
    [ADR-0024](0024-netflix-golden-preserved.md).
  - Fork/upstream bit-exactness on ADM is preserved — a future
    `/sync-upstream` that includes Netflix's own Netflix#955 fix
    will produce a clean diff against the fork's code, and the
    matching Netflix-authored test update will come with it.
  - Downstream integrators calibrating on published VMAF numbers
    keep the numbers they expect.
- **Negative**:
  - ADM scales 1–3 carry a small (≈1 LSB per summed term),
    documented numerical bias relative to the intended integer
    rounding. The bias is part of the upstream-published VMAF
    contract; flagged here so future maintainers don't hunt for
    a "bug" that is deliberately retained.
  - Anyone building a new extractor on top of ADM's integer
    pipeline must read the in-file warning comment — the
    rounding term is sign-negated, not just imprecise.
- **Neutral / follow-ups**:
  - Rebase-notes entry 0048 pins the three invariants that
    matter on upstream sync (file scope, bug site, golden
    linkage).
  - The existing Netflix golden tests transitively gate this
    decision — if the bias ever changes (e.g. a SIMD variant
    drifts), `meson test -C build` catches it.
  - If Netflix closes #955 with a fix, remove the in-file
    warning comment and this ADR's "deferred" status flips to
    "Superseded by …" with a follow-up ADR explaining the
    coordinated golden-number update.

## Verification

This ADR is a **documentation-only** landing. Numerical
verification is the *absence* of change:

- `meson test -C build` → 35/35 pass (no numerical delta).
- `meson test -C build --suite=fast` → green.
- `make test-netflix-golden` → the three Netflix CPU golden
  pairs produce bit-identical VMAF scores (76.66890… on
  `src01_hrc00/01_576x324`) before and after this PR.
- `clang-tidy -p build core/src/feature/integer_adm.c` →
  zero new warnings (the overflow-site is narrated, not
  suppressed; clang-tidy does not flag `(1u << 31)` assigned
  into `int32_t` as a warning on this codebase).

## References

- Upstream issue:
  [Netflix/vmaf#955](https://github.com/Netflix/vmaf/issues/955)
  ("Weird rounding in i4_adm_cm"), OPEN since 2020; no
  maintainer response as of 2026-04-24.
- Fork PR #44 — `feat(libvmaf/feature): port upstream ADM
  updates (Netflix 966be8d5)`. Ported 8 ADM files wholesale,
  preserving this overflow verbatim.
- [ADR-0024](0024-netflix-golden-preserved.md) — project hard
  rule: never modify Netflix golden `assertAlmostEqual` values.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local
  PR deliverables checklist (driving this ADR's existence).
- [ADR-0142](0142-port-netflix-18e8f1c5-vif-sigma-nsq.md) —
  Netflix-authority carve-out precedent (would apply *if*
  Netflix#955 ever closed upstream with a matching golden-
  number update).
- [rebase-notes 0048](../rebase-notes.md) — upstream-sync
  invariants for this decision.
- Backlog: `.workingdir2/BACKLOG.md` T1-8.
- User direction 2026-04-24 popup: "T1-8 ADM i4_adm_cm rounding
  re-verify (Netflix#955)" → "Document + defer (Recommended)".
