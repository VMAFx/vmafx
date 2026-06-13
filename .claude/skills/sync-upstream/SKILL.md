---
name: sync-upstream
description: Reconcile fork master with Netflix/vmaf master. Detects the fork's port-only topology (no shared merge-base) and emits a coverage report; falls back to merge-based sync when the histories are actually connected.
---

<!-- markdownlint-disable MD029 MD046 -->

# /sync-upstream

## Invocation

```text
/sync-upstream [--open-pr]
```

## Background — why pre-flight matters

The fork has been maintained with a **port-only** strategy: each upstream
commit is cherry-picked, rebranded (`feat: port upstream X`, subject preserved),
and squash-merged. That produces commit SHAs on fork master that do **not**
descend from the upstream-master SHAs they originated from. As a result,
`git merge-base master upstream/master` returns empty — the histories are
formally unrelated.

Running a bare `git merge upstream/master --no-ff` in that state requires
`--allow-unrelated-histories` and produces thousands of spurious conflicts
(every fork-local file collides with its upstream counterpart by path,
regardless of content). The skill must detect this topology before attempting
a merge.

## Steps

1. **Pre-flight: topology detection.**

   ```bash
   git fetch upstream
   mb=$(git merge-base master upstream/master 2>/dev/null) || true
   ```

   - **If `mb` is empty** → port-only topology. Go to step 2a (coverage check).
   - **If `mb` is non-empty** → merge-based topology. Go to step 2b (classic merge).

2a. **Port-only coverage check.** For each upstream commit reachable from
    `upstream/master` since the last fork-side port anchor:
    ```bash
    # Derive a reasonable upper bound: the last 50 upstream commits.
    # Expand as needed; subjects are the match key.
    git log upstream/master --pretty=format:'%H%x09%s' -50 > /tmp/sync-upstream-candidates.tsv
    ```

    **Pass 1 — subject-line match (cheap, catches PRs that cite the upstream
    SHA in their subject):**
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
    ports where the same change was made by a fork dev without citing
    upstream).** PR #295's 2026-05-02 sync report missed 4 of 6 candidates
    that turned out to be already on the fork — Pass 2 catches that class:
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
    The 80% threshold is empirical: at `ratio=80%+`, the upstream commit's
    semantic content is overwhelmingly already in fork master; at `<50%` the
    commit is genuinely missing. The 50–80% band is fuzzy and requires
    eyeballing the commit's substance vs the fork's tree.

    **Categorise the final output:**
    - `PORTED` (Pass 1 hit) — fork commit cites the SHA in its subject.
    - `PORTED-SILENTLY` (Pass 2 hit) — semantic content present, no SHA
      citation. Surface in the report so the maintainer can decide whether
      to backfill a citation. NOT a `/port-upstream-commit` candidate.
    - `UNPORTED` (neither pass hit) — genuinely missing. Recommend
      `/port-upstream-commit <sha>`.

    - If every listed commit is `PORTED` or `PORTED-SILENTLY` → exit with
      `sync-upstream: no action — fork at parity with upstream/master @ <tip-sha>`.
    - If any are `UNPORTED` → list them and recommend
      `/port-upstream-commit <sha>` for each. Do NOT attempt a merge. This is
      the expected outcome in port-only mode.

2b. **Merge-based sync.** Only reached when `mb` is non-empty:
    ```bash
    git switch -c sync/upstream-$(date +%Y%m%d) master
    git merge upstream/master --no-ff
    ```
    Conflict policy (matches D16):
    - Fork wins for: `.github/`, `README.md`, `CLAUDE.md`, `AGENTS.md`,
      `.claude/`, `Dockerfile*`, `core/meson_options.txt`, anything under
      `core/src/cuda/`, `core/src/sycl/`, `core/src/feature/{cuda,sycl}/`,
      `core/src/feature/x86/`, `core/src/feature/arm64/`.
    - Upstream wins for: feature metric code not touched by the fork —
      identify by checking `git log --follow origin/master -- <path>` for
      any fork commits.
    - Manual resolution required: `core/include/libvmaf/libvmaf.h`,
      `core/src/libvmaf.c`, `core/meson.build`,
      `core/tools/cli_parse.c`, `core/tools/vmaf.c`.
    For manual conflicts, STOP and surface them with `file:line` context. Do
    NOT resolve.

3. **On clean merge (step 2b only):** `/build-vmaf --backend=cpu`,
   `meson test -C build`, `/cross-backend-diff` on the normal Netflix pair.

4. **If `--open-pr` (step 2b only):** `gh pr create` with title
   `chore(upstream): sync to upstream/master @ <sha>` and body including
   upstream commit count, conflict summary, and test results.
   - In port-only mode (step 2a), `--open-pr` is a no-op when coverage is
     complete; when gaps exist, the recommendation is one PR per
     `/port-upstream-commit <sha>` invocation, not a single sync PR.

## Guardrails

- Refuses to run if working tree is dirty.
- Refuses to open a PR if the Netflix CPU golden tests fail after merge.
- Never `git push --force`.
- Pre-flight short-circuit is mandatory — the classic merge step MUST NOT
  be reached when no merge-base exists, even under operator override, because
  the `--allow-unrelated-histories` fallback has been known to corrupt
  hand-curated port history (see ADR-0028 and the post-mortem thread in
  [docs/rebase-notes.md](../../../docs/rebase-notes.md)).
