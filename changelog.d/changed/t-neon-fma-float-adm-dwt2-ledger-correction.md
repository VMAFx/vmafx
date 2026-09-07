- Correct the `docs/state.md` ledger row for
  `T-NEON-FMA-FLOAT-ADM-DWT2-2026-06-06`, which was self-contradictory: a
  2026-06-27 note said the bug "stays Open" while its Recently-closed row
  recorded a 2026-08-30 closure that described only the integer-path
  dropped-tap defect and cited a branch name instead of a commit. Re-verified
  against `master`: the FMA-safe follow-up landed in `a6c4dfffb` (PR #853,
  dedicated non-contracting `core/src/feature/arm64/float_adm_dwt2_neon.c` TU
  plus the `adm_dwt2_dispatch()` rewire in `core/src/feature/adm.c`), the
  integer `idx < 3` dropped filter tap in `adm_dwt2_8_neon` was fixed by
  `a013c1410` (PR #1134) and hardened by `89a8e3258` (PR #1154) and
  `6d61106ed` (PR #1156), and `core/test/test_float_adm_dwt2_neon.c` now gates
  NEON-vs-scalar bit-exactness in the default `fast` suite. The row is
  rewritten in past tense with real commit shas and the stale "stays Open"
  note is marked superseded. Documentation only — no code or behaviour change.
