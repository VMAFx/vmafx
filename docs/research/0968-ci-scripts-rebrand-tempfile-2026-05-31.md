<!-- markdownlint-disable MD013 -->
# Research digest — CI scripts Round 26 audit (D.1 + D.2)

**Date**: 2026-05-31
**ADR**: [ADR-0968](../adr/0968-ci-scripts-rebrand-proof-and-tempfile-trap.md)
**Scope**: `scripts/ci/assertion-density.sh` (D.1) and
`scripts/release/concat-changelog-fragments.sh` (D.2)

---

## D.1 — assertion-density.sh silent skip after copyright rebrand

### Root cause

`assertion-density.sh` at line 22 used a literal grep:

```bash
grep -q "Lusoris and Claude"
```

The copyright-rebrand decision (2026-05-27; memory: `project_copyright_lusoris_only`)
changed the canonical copyright line for new fork-added files from
`Copyright YYYY Lusoris and Claude (Anthropic)` to `Copyright YYYY Lusoris`.
Any file written after the rebrand foundation PR lands would fail the literal
match, causing the script to exit 0 at line 30 with:

```text
assertion-density: no fork-added files found; skipping
```

This exit path bypasses the entire assertion-density gate — the script
reports success without scanning any files, hiding any density violations
in post-rebrand code.

### Impact

- All post-rebrand fork-added `.c` / `.cpp` files excluded from the
  Power of 10 rule-5 assertion-density CI gate.
- Pre-rebrand files (those already in the tree using the legacy format)
  continue to be scanned correctly.
- The bug is latent until the rebrand foundation PR merges; at that point
  it becomes immediately exploitable by any new PR that does not add
  `assert()` calls.

### Fix

Replace the single-literal grep with an extended-regex that accepts both
formats:

```bash
grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"
```

Pattern analysis:

- `Lusoris and Claude` — matches legacy headers (`Copyright 2025 Lusoris and Claude (Anthropic)`)
- `Copyright [0-9]+ Lusoris` — matches new headers (`Copyright 2026 Lusoris`)
- Netflix-only headers (`Copyright 2016-2024 Netflix, Inc.`) match neither
  pattern and continue to be excluded correctly.
- A hypothetical future format change (e.g. different year format) would
  need to extend the regex again; the `[0-9]+` year match is intentionally
  broad to avoid this triggering again on a minor date-formatting change.

### Verification

Six tests added in `scripts/ci/tests/test-assertion-density.sh`:

- T1: legacy header matched
- T2: new-format header matched (was the failing case before the fix)
- T3: Netflix-only header skipped
- T4: no-header file skipped
- T5: mixed legacy + new headers — both matched
- T6: Lusoris + Netflix mix — only Lusoris file matched

Test run: all 6 pass.

---

## D.2 — concat-changelog-fragments.sh tempfile leak

### Root cause

The `--write` path allocates two tempfiles immediately before the awk
pipeline:

```bash
tmp_body="$(mktemp)"
tmp_out="$(mktemp)"
```

The original cleanup was a single `rm -f "$tmp_body"` at line 153,
placed after the `mv "$tmp_out" "$CHANGELOG"` call. Under `set -euo pipefail`:

- If the awk pipeline fails (e.g. malformed input, missing binary), the
  script exits before the `rm -f` line is reached — both `$tmp_body` and
  `$tmp_out` remain in `/tmp`.
- If `mv` fails (e.g. read-only filesystem, wrong permissions), same
  outcome — both files leak.
- Even on a clean run, `$tmp_out` was never explicitly removed (only
  `$tmp_body` was cleaned up); the `mv` atomically replaces `CHANGELOG.md`
  but the temp path ceases to exist after the rename, so the inode is
  cleaned up by the OS. However, a `mv` failure leaves `$tmp_out` behind.

The POSIX semantics of `trap ... EXIT` guarantee the handler fires on
any exit cause (normal, error via `set -e`, and most signals), making it
the correct mechanism for unconditional tempfile cleanup.

### Fix

```bash
tmp_body="$(mktemp)"
tmp_out="$(mktemp)"
trap 'rm -f "$tmp_body" "$tmp_out"' EXIT   # ← added
```

The redundant post-`mv` `rm -f "$tmp_body"` is removed; the trap is the
single cleanup point.

### Verification

Four tests added in `scripts/release/tests/test-concat-changelog-fragments.sh`:

- T1: static check — EXIT trap present in script source
- T2: static check — only one `rm -f "$tmp_body"` reference (trap only)
- T3: dynamic check — awk failure via `PATH` shim; verified no new
  `tmp.*` files in `/tmp` after the script exits non-zero
- T4: happy path — `--write` rewrites `CHANGELOG.md` correctly

Test run: all 5 assertions pass (T3 exercises two assertions).

---

## No-digest-needed items

Both D.1 and D.2 are self-contained correctness fixes in fork-local CI
scripts with no upstream Netflix counterpart. No architectural trade-offs
beyond those documented in the ADR alternatives table. No new dependencies.
