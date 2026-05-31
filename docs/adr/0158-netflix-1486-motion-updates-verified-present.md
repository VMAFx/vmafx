# ADR-0158: Netflix#1486 "Port motion updates" — verified present in fork

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: upstream-port, motion, netflix-upstream, verification

## Context

Backlog item T4-1 called for porting Netflix upstream PR
[#1486 "Port motion updates"](https://github.com/Netflix/vmaf/pull/1486),
merged upstream on 2026-04-20 as two commits:

1. **`a44e5e6`** — `libvmaf/feature: port motion updates, bugfix for
   edge mirroring`. Adds `motion_max_val` option (default 10000.0;
   clips motion scores above), fixes the `edge_8` mirroring bug
   (`height - (i_tap - height + 1)` → `height - (i_tap - height + 2)`)
   across the scalar + AVX2 + AVX-512 paths, adds
   `VMAF_integer_feature_motion3_score` output in
   `extract_force_zero`, and moves SIMD dispatch out of `init`.

2. **`62f47d5`** — `loosen assertion precision for motion mirror
   bugfix`. Netflix golden `assertAlmostEqual` updates across
   `python/test/{feature_extractor_test.py,quality_runner_test.py,
   vmafexec_feature_extractor_test.py,vmafexec_test.py}` — the
   numerical-gate coordinated with commit 1.

A verification pass confirmed that **both commits are already fully
present in the fork's `master`**:

- `edge_8` `+ 2` fix: present in
  [`core/src/feature/integer_motion.c:240`](../../core/src/feature/integer_motion.c),
  [`x86/motion_avx2.c:147`](../../core/src/feature/x86/motion_avx2.c),
  [`x86/motion_avx512.c:147`](../../core/src/feature/x86/motion_avx512.c).
- `motion_max_val` option: present at `integer_motion.c:57,118-120`
  and in the options table.
- `VMAF_integer_feature_motion3_score`: present at
  [`core/src/feature/alias.c:92`](../../core/src/feature/alias.c)
  and in `integer_motion.c` output plumbing.
- Netflix golden updates: the 73 `(results[N][key], value, places=)`
  tuples from upstream commit `62f47d5` are present in the fork's
  python tests (verified programmatically by parsing all four
  files for the exact triples).

The fork's port didn't land as a single PR matching Netflix#1486 1:1.
Fork PR #45 (`9371a0aa feat(libvmaf/feature): port upstream motion
updates (Netflix PR #1486)`) was opened but never merged; its
substance was folded into `master` incrementally via later motion3 /
blend / five-frame-window / moving-average commits that went through
separate PRs. The end state is equivalent: every line the upstream
patch would add is in the fork today.

Why this matters: T4-1 on the backlog was sized M (motion metric
math + cross-backend-diff + Netflix golden gate). Re-porting would
be a no-op at best and a conflict-resolution exercise at worst. The
verification + paper trail is the right closing move.

## Decision

**Close T4-1 as "verified present; no port PR needed."** This ADR +
a rebase-notes entry document the coverage so that future
`/sync-upstream` runs don't re-discover Netflix#1486 as pending.

No code change. No CHANGELOG entry (no user-visible delta —
everything upstream shipped is already in the fork's released
binaries). Backlog item flipped to done with a verification
reference.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Doc-only close (this ADR)** | Paper trail for future sync-upstream; no code churn; honest representation of current state | Small docs-only PR adds a commit to master | **Chosen** via popup — next `/sync-upstream` gets the coverage context without re-auditing |
| **Open a port PR anyway** | Keeps 1:1 commit-to-port mapping across upstream/fork | No-op diff; pure churn; conflict resolution with existing motion3 / blend work | Rejected — wasted cycles |
| **In-session close (no PR)** | Zero commits | No tracked paper trail; future sync-upstream re-checks #1486 from scratch | Rejected via popup |
| **Supersede fork PR #45** | Would close the unmerged PR explicitly | PR #45 is already stale (last activity >6 months); GitHub auto-closes on push reference | Not needed — the branch is long-dead |

## Consequences

- **Positive**:
  - `/sync-upstream` won't re-flag Netflix#1486 as a pending port.
  - Future rebase-audits have a cite for "this upstream PR is
    already absorbed via fork-native work, no cherry-pick required."
  - Backlog tier T4 shrinks by one.
- **Negative**: None.
- **Neutral / follow-ups**:
  - Fork-local motion extensions (five-frame-window,
    moving-average, blend, fps_weight) are additions on top of
    Netflix#1486; future upstream changes to motion extractor
    internals may conflict with them. That invariant is noted
    in `core/src/feature/AGENTS.md` under the existing
    motion_v2 NEON entry.
  - The pre-existing `readability-braces-around-statements`
    warnings on `integer_motion.h:38-48` (noticed during
    verification) are upstream style carried forward — ADR-0141
    would catch them on the next touch of that header. Not a
    blocker here since nothing changed.

## Verification

- `ninja -C build && meson test -C build` → **35/35 pass** (no
  delta vs pre-verification master).
- Markers verified present via:

  ```bash
  grep -n "height - (i_tap - height + 2)\|motion_max_val\|VMAF_integer_feature_motion3_score" \
      core/src/feature/integer_motion.c \
      core/src/feature/alias.c \
      core/src/feature/x86/motion_avx2.c \
      core/src/feature/x86/motion_avx512.c
  ```

- 73-triple golden-value match on python tests verified by
  programmatic AST-style scan (details in subagent report).
- `git status` → clean; this PR contributes no code hunks.

## References

- Upstream PR:
  [Netflix/vmaf#1486](https://github.com/Netflix/vmaf/pull/1486)
  ("Port motion updates"), MERGED 2026-04-20.
- Upstream commits: `a44e5e6`, `62f47d5` (in
  `Netflix/vmaf@master`).
- Fork PR #45 (never merged;
  [9371a0aa](https://github.com/VMAFx/vmafx/pull/45)) — tracked
  the original port attempt; substance landed via incremental
  fork-native commits instead.
- [ADR-0024](0024-netflix-golden-preserved.md) — Netflix golden
  gate.
- [ADR-0142](0142-port-netflix-18e8f1c5-vif-sigma-nsq.md) —
  Netflix-authority carve-out for coordinated golden-value
  updates; would apply if a real port PR were needed here.
- [ADR-0145](0145-motion-v2-neon-bitexact.md) — fork-local
  `motion_v2_neon.c`; unrelated to this upstream PR but
  touches the same file family on rebase.
- [rebase-notes 0051](../rebase-notes.md) — sync-upstream
  invariants.
- Backlog: `.workingdir2/BACKLOG.md` T4-1.
- User direction 2026-04-24 popup: "T4-1 Port motion updates
  Netflix#1486" → "Doc-only PR (Recommended)".
