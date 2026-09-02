<!-- markdownlint-disable MD013 MD060 -->
# ADR-0968: CI scripts — rebrand-proof assertion-density grep + tempfile EXIT trap in changelog concat (Round 26 audit D.1 + D.2)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `docs`

## Context

Round 26 CI-scripts audit identified two distinct bugs:

**D.1 — assertion-density.sh silent skip after rebrand.**
`scripts/ci/assertion-density.sh` identified fork-added files by checking
whether their first 20 lines contained the literal string `"Lusoris and Claude"`.
The copyright-rebrand decision (2026-05-27; memory: `project_copyright_lusoris_only`)
changed new files to use `"Copyright YYYY Lusoris"` without the `"and Claude
(Anthropic)"` suffix. After the foundation rebrand PR lands, every new fork-added
file carries only the new format, so the old `grep -q "Lusoris and Claude"` never
matches. With no files found the script exits 0 with "no fork-added files found;
skipping" — silently bypassing the NASA/JPL Power of 10 rule-5 assertion-density
gate for all post-rebrand code.

**D.2 — concat-changelog-fragments.sh tempfile leak.**
The `--write` path in `scripts/release/concat-changelog-fragments.sh` allocates
two tempfiles (`tmp_body` and `tmp_out`) via `mktemp` but only cleans up
`tmp_body` in a post-`mv` `rm -f` that is never reached when the script
aborts early. Under `set -euo pipefail` any failure in the awk pipeline or the
`mv` call causes an immediate exit, leaking both files into `/tmp` for the
lifetime of the CI runner.

## Decision

**D.1**: Replace the literal `grep -q "Lusoris and Claude"` with an extended-regex
form that matches both the legacy and current copyright formats:

```bash
grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"
```

This matches all existing in-tree files (legacy format) and all future files
written after the rebrand (current format), without touching the detection
logic for upstream Netflix files (which carry neither pattern).

**D.2**: Add `trap 'rm -f "$tmp_body" "$tmp_out"' EXIT` immediately after the
two `mktemp` calls in the `--write` branch, and remove the now-redundant manual
`rm -f "$tmp_body"` at the end of the branch. The trap fires on any exit —
normal, `set -e`, signal — making cleanup unconditional.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| D.1: Broaden to `grep -q "Lusoris"` | Simpler regex | Would incorrectly include any file that mentions "Lusoris" in a comment but is not a Lusoris-copyright file | Too broad — false positives possible |
| D.1: Add a separate scan pass for each format | No regex complexity | Two passes over the file list; more code | Unnecessary; the combined `-E` regex is readable and correct |
| D.2: Use a dedicated `cleanup()` function called via trap and on each error path | Slightly more explicit | Adds boilerplate with no semantic benefit over a direct trap | Not needed; trap-on-EXIT is idiomatic and sufficient |
| D.2: Use `mktemp` with a named prefix matching the script name so leaked files are identifiable | Eases debugging of leaks | Does not fix the leak, only makes it visible | Addressing the symptom, not the cause |

## Consequences

- **Positive**: Assertion-density gate correctly covers all post-rebrand
  fork-added files. Changelog concat script leaves no orphaned tempfiles in
  `/tmp` under any exit path.
- **Negative**: None; both changes are strictly additive or correctness-only.
- **Neutral / follow-ups**: Test coverage added at
  `scripts/ci/tests/test-assertion-density.sh` (T1–T6, covering both header
  formats and exclusion of Netflix/no-header files) and
  `scripts/release/tests/test-concat-changelog-fragments.sh` (T1–T4,
  including simulated awk failure via a `PATH` shim). AGENTS.md invariants
  added to `scripts/ci/AGENTS.md` and `scripts/release/AGENTS.md`.

## References

- Memory: `project_copyright_lusoris_only` — copyright-rebrand decision 2026-05-27
- Round 26 CI-scripts audit findings D.1 and D.2
- NASA/JPL Power of 10 rule 5 (assertion density) — `docs/principles.md §1.2`
- [ADR-0100](0100-project-wide-doc-substance-rule.md) — per-surface doc bar
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverable PR rule
