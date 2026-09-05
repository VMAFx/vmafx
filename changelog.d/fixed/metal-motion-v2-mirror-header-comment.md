- **Stale bug-ledger row and stale header comment for the Metal
  `integer_motion_v2` mirror fold.** `docs/state.md` still listed
  `T-METAL-MOTION-V2-MIRROR-OFF-BY-ONE-2026-09-03` under "Open
  bugs", claiming `core/src/feature/metal/integer_motion_v2.metal`
  folded the high boundary with `2 * sup - idx - 1` instead of the
  scalar reflect-101 `2 * size - idx - 2`. The row was written on
  2026-09-03 and the fix landed on 2026-09-04 in `71da046db`
  (PR #1223, ADR-1166), which replaced the helper with the iterated
  fold `2 * (sup - 1) - idx` — algebraically the CPU form — and so
  also removed the residual out-of-range single-bounce read on
  small dimensions. The row is moved to "Recently closed" with the
  verification recorded, so the next session does not re-open a
  numeric-parity investigation into already-correct code. The
  file's top-of-file block comment, which still described the
  removed `- 1` convention and contradicted the corrected
  `mv2_mirror` comment below it, is rewritten to reflect-101. No
  Metal code path reads the comment; scores, snapshots and the
  Netflix golden gate are unaffected.
