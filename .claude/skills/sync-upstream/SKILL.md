---
name: sync-upstream
description: Reconcile fork master with Netflix/vmaf master. Detects the fork's port-only topology (no shared merge-base) and emits a coverage report; falls back to merge-based sync when the histories are connected.
---
<!-- markdownlint-disable MD029 MD046 -->

# /sync-upstream

## Invocation

```text
/sync-upstream [--open-pr]
```

## Background — why pre-flight matters

Fork maintained with **port-only** strategy: upstream commits cherry-picked,
rebranded (`feat: port upstream X`, subject preserved), squash-merged.
Fork master commit SHAs do **not** descend from upstream-master SHAs ->
`git merge-base master upstream/master` returns empty (histories formally
unrelated).

Bare `git merge upstream/master --no-ff` requires
`--allow-unrelated-histories`, produces thousands of spurious conflicts
(every fork-local file collides with upstream counterpart by path,
regardless of content). Skill must detect topology before merge attempt.

## Steps

1. **Pre-flight: topology detection.**

   ```bash
   git fetch upstream
   mb=$(git merge-base master upstream/master 2>/dev/null) || true
   ```

   - **If `mb` is empty** -> port-only topology. Go to step 2a (coverage check).
   - **If `mb` is non-empty** -> merge-based topology. Go to step 2b
     (classic merge).

2a. **Port-only coverage check.** Upstream commits reachable from
    `upstream/master` since last fork-side port anchor:

    ```bash
    # Derive a reasonable upper bound: the last 50 upstream commits.
    # Expand as needed; subjects are the match key.
    git log upstream/master --pretty=format:'%H%x09%s' -50 > /tmp/sync-upstream-candidates.tsv
    ```

    **Pass 1 — subject-line match (catches PRs citing upstream SHA in
    subject):**

    ```bash
    while IFS=$'\t' read -r sha subj; do
      if git log master --pretty=format:'%s' \
           | grep -Fxq "$subj"; then
        echo "PORTED    $sha  $subj"
      else
        echo "UNPORTED  $sha  $subj"
      fi
    done < /tmp/sync-upstream-candidates.tsv
    ```

    **Pass 2 — content-hash similarity for `UNPORTED` rows (catches silent
    ports without upstream citation).** PR #295 2026-05-02 sync report
    missed 4 of 6 candidates already on fork -> Pass 2 catches class:

    ```bash
    # For each UNPORTED upstream commit, extract added/changed identifiers
    # and check if they exist in fork master's HEAD. If yes, it's silently
    # ported (the change shipped without an SHA citation in the commit subject).
    for sha in $(awk '/^UNPORTED/ {print $2}' /tmp/sync-upstream-pass1.tsv); do
        # Extract added/changed identifiers from the upstream commit
        # (function names, variable names, string literals it INTRODUCES).
        idents=$(git show "$sha" --no-color --pretty=format:'' \
                 | grep -E '^\+[^+]' \
                 | grep -oE '[a-zA-Z_][a-zA-Z0-9_]{4,}' \
                 | sort -u)
        # Skip if no useful identifiers (e.g. doc-only or formatting commit).
        [ -z "$idents" ] && continue
        # Probe fork master for at least 80% of the identifiers.
        n_total=$(echo "$idents" | wc -l)
        n_present=$(echo "$idents" | xargs -I{} sh -c \
            'git grep -lq "{}" -- master 2>/dev/null && echo present' | wc -l)
        ratio=$((n_present * 100 / n_total))
        if [ "$ratio" -ge 80 ]; then
            echo "PORTED-SILENTLY    $sha  ratio=$ratio% n_total=$n_total"
        else
            echo "UNPORTED           $sha  ratio=$ratio%"
        fi
    done
    ```

    80% threshold empirical:
    - `ratio=80%+` -> upstream semantic content in fork master.
    - `<50%` -> commit genuinely missing.
    - 50–80% band fuzzy -> eyeball commit substance vs fork tree.

    **Categorise final output:**
    - `PORTED` (Pass 1 hit) -> fork commit cites SHA in subject.
    - `PORTED-SILENTLY` (Pass 2 hit) -> semantic content present, no SHA
      citation. Surface in report for maintainer citation backfill decision.
      NOT `/port-upstream-commit` candidate.
    - `UNPORTED` (neither pass hit) -> genuinely missing. Recommend
      `/port-upstream-commit <sha>`.

    - All commits `PORTED` or `PORTED-SILENTLY` -> exit with:
      `sync-upstream: no action — fork at parity with upstream/master @ <tip-sha>`
    - Any `UNPORTED` -> list, recommend `/port-upstream-commit <sha>` each.
      Do NOT attempt merge. Expected outcome in port-only mode.

2b. **Merge-based sync.** Reached only when `mb` non-empty:

    ```bash
    git switch -c sync/upstream-$(date +%Y%m%d) master
    git merge upstream/master --no-ff
    ```

    Conflict policy (matches D16):
    - Fork wins: `.github/`, `README.md`, `CLAUDE.md`, `AGENTS.md`, `.claude/`,
      `Dockerfile*`, `core/meson_options.txt`, under `core/src/cuda/`,
      `core/src/sycl/`, `core/src/feature/{cuda,sycl}/`,
      `core/src/feature/x86/`, `core/src/feature/arm64/`.
    - Upstream wins: feature metric code not touched by fork (check
      `git log --follow origin/master -- <path>` for fork commits).
    - Manual resolution required: `core/include/libvmaf/libvmaf.h`,
      `core/src/libvmaf.c`, `core/meson.build`, `core/tools/cli_parse.cpp`,
      `core/tools/vmaf.cpp`.
    - Manual conflicts: STOP, surface with `file:line` context. Do NOT resolve.

3. **On clean merge (step 2b only):** `/build-vmaf --backend=cpu`,
   `meson test -C build`, `/cross-backend-diff` on normal Netflix pair.

4. **If `--open-pr` (step 2b only):** `gh pr create` with title
   `chore(upstream): sync to upstream/master @ <sha>`, body including
   upstream commit count, conflict summary, test results.
   - Port-only mode (step 2a): `--open-pr` = no-op when coverage complete;
     gaps exist -> recommend 1 PR per `/port-upstream-commit <sha>` invocation,
     not single sync PR.

## Guardrails

- Refuses run if working tree dirty.
- Refuses PR open if Netflix CPU golden tests fail after merge.
- Never `git push --force`.
- Pre-flight short-circuit mandatory: classic merge step MUST NOT be reached
  when no merge-base exists, even under operator override
  (`--allow-unrelated-histories` fallback corrupts hand-curated port history;
  see ADR-0028, post-mortem thread in
  [docs/rebase-notes.md](../../../docs/rebase-notes.md)).
