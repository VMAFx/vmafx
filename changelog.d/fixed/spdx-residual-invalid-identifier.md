- **92 files declared an SPDX licence identifier that does not exist.**
  `BSD-3-Clause-Plus-Patent` is not on the SPDX licence list — the only
  patent-bearing BSD identifier is `BSD-2-Clause-Patent`, and there is no
  three-clause patent variant at all, so the string is a mash-up. One
  further occurrence used `BSD+Patent`, the informal spelling, which is
  likewise not an identifier. A 1.0.0 that declares a non-existent
  identifier fails every downstream REUSE and SPDX validator.
  1,027 tracked files carried it; PR #1457's EUPL-1.2 relicensing
  replaces it on 935 of them as a side effect, and its provenance vetoes
  leave the rest — documentation, `deploy/` and `config/` YAML,
  `.claude/` skill templates, `ai/` scripts, and the OCI
  `org.opencontainers.image.licenses` labels in the Dockerfiles, which
  carried the same invalid expression. Those are now
  `BSD-2-Clause-Patent`, the licence in the root `LICENSE` and already
  correctly used by 367 files in this repository — so this corrects an
  inconsistency rather than asserting anything new. Only the identifier
  token was rewritten: the Go files' dual `... OR MIT` structure is
  preserved, and no prose that discusses the old identifier was touched.
  **This does not make the tree SPDX-clean on its own** — 938
  occurrences remain and clear only when #1457 lands. ADR-1255.
