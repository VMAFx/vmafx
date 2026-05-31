- Apply `markdownlint-cli2 --fix` to the docs corpus with an ultra-safe
  allow-list config (`MD012`, `MD022`, `MD031`, `MD032`, `MD058` — pure
  blank-line normalisation rules only). 253 files touched; +1,266 / -225
  lines; `git diff -w` is empty (no content change). Total markdownlint
  violation count drops from 21,775 to 19,822 (-9%). The remaining tail
  (MD060 table-column-style, MD013 line-length, MD050 strong-style)
  cannot be safely auto-fixed at corpus scale — `--fix` empirically
  corrupts C identifiers (`__restrict__`, `__ldg`), assembly tab
  indentation, shell-prompt examples, and reference-style link
  definitions. Honours ADR-0866's "no `--fix` for default rules" policy.
  See [ADR-0979](../../docs/adr/0979-markdown-lint-blank-line-sweep.md).
