<!-- markdownlint-disable MD060 -->
# Upstream-backlog re-audit — 2026-04-29 (T7-4)

> Quarterly upstream-backlog re-audit per
> [`.workingdir2/BACKLOG.md`](../.workingdir2/BACKLOG.md) row T7-4.
> Companion to the 2026-04-18 snapshot in
> [`.workingdir2/analysis/upstream-backlog-audit.md`](../.workingdir2/analysis/upstream-backlog-audit.md)
> (local-only). Doubles as the deep-dive **research digest** for this
> PR per [ADR-0108](adr/0108-deep-dive-deliverables-rule.md). Next
> re-audit due **2026-07-29**.

## Summary

- **12 upstream commits** walked since the previous fork-side
  upstream port boundary (`798409e3` and below), `git fetch upstream
  master` snapshot taken 2026-04-29.
- **8 already on fork** — content cherry-picked, ported, or
  consciously diverged-from with an Accepted ADR.
- **4 flagged for fork action** — surfaced as recommended new T-rows
  below.
- **0 new bug rows** for [`docs/state.md`](state.md); no upstream
  commit ruled in or out a bug that affects the fork's user-visible
  surface.
- **No decision-matrix needed:** scheduled audit, not a non-trivial
  decision per [CLAUDE.md §12 rule 8](../CLAUDE.md).

## Method

1. `git fetch upstream master` (Netflix/vmaf).
2. Walk `git log upstream/master --oneline 798409e3^..upstream/master`
   — `798409e3` is the lower bound because PR #181 (`6eab09c0`) is
   the most recent direct `chore(upstream): port` commit on fork
   `master` and ports `798409e3` together with `314db130`.
3. Classify each commit into one of:
   - **already-on-fork** — landed via cherry-pick, port, or covered
     by an Accepted ADR;
   - **port** — bug-fix / build-fix worth porting (new T-row);
   - **defer** — recognised, out-of-scope right now;
   - **diverged** — fork has consciously diverged with an ADR.
4. Cross-check fork files (`core/src/feature/`,
   `core/include/`, `.github/workflows/`) for the per-commit
   marker symbols.

## Per-commit triage

| upstream-sha | subject | fork-status | recommended-action | T-row |
|---|---|---|---|---|
| `c70debb1` | libvmaf/test: port new adm/vif/speed tests | port | port the adm + vif test deltas; speed tests pend on T-NEW-1 | T-NEW-2 |
| `314db130` | libvmaf/feature: remove empty translation unit `all.c` | already-on-fork | none — landed via PR #181 (`6eab09c0`) | — |
| `9dac0a59` | libvmaf/feature: update alias map for cambi/speed | partial | cambi half is on fork (PR #160 `79288e8d`); speed half pends on T-NEW-1 | T-NEW-1 |
| `8c645ce3` | feature/vif: port several feature extractor options | diverged | none — Research-0024 + ADR-0142/0143/0144 record deliberate divergence; fork keeps closed-enum kernelscale | — |
| `8a289703` | adm: add fallback for `extract_epi64` for 32-bit | port | small portability fix; aligns with fork's i686 build job (ADR-0151) | T-NEW-3 |
| `1b6c3886` | x86/cpu: remove limit of avx+ on 32-bit | port | partner to `8a289703`; verify against fork's cpu-feature dispatch | T-NEW-3 |
| `f6d6dde1` | gh: add 32-bit cross build to CI | already-on-fork | none — fork has its own i686 build-only matrix row (PR #87 `978f9583`, ADR-0151) | — |
| `b949cebf` | feature/motion: port several feature extractor options | diverged | none — Research-0024 covers; fork kept its own divergence per ADR-0145 | — |
| `4dcc2f7c` | feature/float_adm: port several feature extractor options | diverged | none — Research-0024 covers; fork's `float_adm` divergence accepted | — |
| `2c9bb74e` | feature/cambi: add `effective_eotf` | already-on-fork | none — landed via PR #160 (`79288e8d`) | — |
| `eb88c009` | build(deps): bump `actions/upload-artifact` 5→7 | already-on-fork | none — fork manages action versions via Dependabot | — |
| `798409e3` | Fix null deref crash on `prev_ref` update in pure CUDA pipelines | already-on-fork | none — landed via PR #181 (`6eab09c0`) | — |

### Status legend

- **already-on-fork** — content is on `master` (cherry-pick, port, or
  fork-equivalent infrastructure).
- **port** — recommended action: open a `port-upstream-commit` PR.
- **diverged** — fork has consciously chosen a different design;
  Accepted ADR documents the rationale; no action required.

## Recommended new T-rows

The following follow-up entries are recommended for the next
[`.workingdir2/BACKLOG.md`](../.workingdir2/BACKLOG.md) refresh:

- **T-NEW-1 — port `feature/speed`** (`d3647c73` `speed_chroma` +
  `speed_temporal`, `9dac0a59` alias-map for speed). The fork does
  not currently have a `speed*` extractor in
  `core/src/feature/`; upstream PR #1433 also adds a related
  speed-extractor surface. Investigate whether to port the upstream
  C extractor wholesale, or to absorb it into the fork's tiny-AI
  speed metric. Pairs with audit row 1 / 3.
- **T-NEW-2 — port adm + vif test deltas from `c70debb1`.** The
  speed half of `c70debb1` blocks on T-NEW-1; the adm + vif halves
  should drop in cleanly on top of fork's existing port chain
  (Research-0024). Goal: keep `core/test/test_*.c` test-data
  parity with upstream wherever the fork's algorithmic content
  matches.
- **T-NEW-3 — port 32-bit ADM/cpu fallbacks** (`8a289703` +
  `1b6c3886`). Pairs with the existing i686 build-only CI matrix
  row (PR #87, ADR-0151). Currently the fork's i686 lane is
  build-only; these two commits add the runtime-correctness pieces
  (32-bit `extract_epi64` fallback, lifting the AVX-on-32-bit
  guard) that would let the i686 lane be **run** as well.
- **T-NEW-4 — schedule next quarterly re-audit for 2026-07-29.**
  Update T7-4's "next" date in `BACKLOG.md`.

## What this audit did *not* cover

- **Open Netflix issues / PRs.** The 2026-04-18 dossier in
  [`.workingdir2/analysis/upstream-backlog-audit.md`](../.workingdir2/analysis/upstream-backlog-audit.md)
  triages every open upstream issue and PR (66 + 35 at that
  snapshot). This re-audit is scoped to **landed upstream commits
  since the last fork-side port boundary**, not the open queue.
  The next snapshot of the open queue is recommended alongside
  T-NEW-4 (re-run with `gh issue list --repo Netflix/vmaf
  --state open --limit 300` per the dossier's "Re-run instructions"
  section).
- **Pre-`798409e3` upstream history.** The 12-commit window
  matches the fork's most recent direct port and the dossier's
  open-PR table. Older upstream commits (e.g. `f3a628b4`,
  `18e8f1c5`, `a44e5e61`, `dae6c1a0`, `f740276a`) are already on
  fork via the Research-0024 chain or earlier ADR-tracked ports
  and were verified via spot-check (`vif_sigma_nsq`, `motion_v2`,
  `VMAF_FEATURE_EXTRACTOR_PREV_REF` all present in
  `core/src/feature/`).

## Reproducer

```bash
# 1. Refresh the upstream tracking ref.
git fetch upstream master

# 2. Walk the window covered by this audit.
git log upstream/master --oneline 798409e3^..upstream/master

# 3. For each commit, cross-check fork master:
git log master --oneline --grep='<sha-or-subject-keyword>'

# 4. For algorithmic divergences, cross-check the ADR / research digest tree:
ls docs/research/ docs/adr/
```

## Re-audit cadence

- Previous: 2026-04-18 (open-queue snapshot).
- This pass: **2026-04-29** (landed-commit re-audit).
- Next: **2026-07-29** (per CLAUDE.md §12 rule 8 + T7-4
  quarterly cadence).
