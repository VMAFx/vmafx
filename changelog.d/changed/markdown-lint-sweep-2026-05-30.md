- **docs(lint):** Markdown lint config tuned for prose-heavy docs/ +
  safe-autofix sweep across `docs/` and `changelog.d/`. Disables 10
  convention-incompatible rules (compact-table style, single-title,
  inline-html, bare-urls, emphasis-style, etc.) and 7 unsafe-to-autofix
  rules (MD004 ul-style, MD007 ul-indent, MD018 atx-spacing, MD027
  blockquote-spacing, MD029 ol-prefix, MD037 emphasis-spacing, MD038
  code-spacing) that would silently rewrite prose tokens —
  `H = 0.5 * log2(...)` → `H = 0.5 *log2(...)`, `#140` (PR number) →
  `# 140` (H1 heading), `+ Netflix corpus` → `- Netflix corpus`. Then
  runs `markdownlint-cli2 --fix` to discharge the safe whitespace
  cleanups (blanks-around-headings/lists/fences, consistent
  list-marker spacing) across 107 files. In-flight DRAFT PR files,
  all `docs/adr/[0-9]{4}-*.md` (immutable once Accepted), and the
  ADR-0108 compliance-audit research file are excluded. Touched-file
  lint-clean rule (CLAUDE.md §12 r12) is now achievable for
  docs-adjacent PRs without 19k pre-existing warnings as a blocker.
  (ADR-0864)
