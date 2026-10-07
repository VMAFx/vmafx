---
paths:
  - scripts/ci/assertion-density.sh
  - scripts/ci/tests/test-assertion-density.sh
invariant: Copyright grep accepts legacy and current Lusoris marker; single-literal pattern silently skips gate.
---
<!-- markdownlint-disable MD013 MD060 -->
# assertion-density.sh — copyright-grep scope (ADR-0968)

`assertion-density.sh` identifies fork-added files by scanning first
20 lines of each `.c` / `.cpp` for Lusoris copyright marker. Grep
pattern **must** accept both legacy format (`Lusoris and Claude
(Anthropic)`) and current post-rebrand format (`Copyright YYYY
Lusoris`). Current pattern:

```text
grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"
```

**Invariant**: do not simplify this to single literal string.
2026-05-27 copyright-rebrand decision (memory: `project_copyright_lusoris_only`)
dropped "and Claude (Anthropic)" from new files; older files in-tree still
carry legacy form. Grep matching only one format causes script
to silently exit 0 ("no fork-added files found; skipping"), bypassing
assertion-density gate for all files carrying other format.

Test coverage: `scripts/ci/tests/test-assertion-density.sh` (T1–T7).
source listing runs in checked command substitution: failing `git
ls-files` or `pelorus_mirror.py filter` exits 2 (T7); inside process
substitution its status was lost and gate passed. fixture repos carry
real filter.
