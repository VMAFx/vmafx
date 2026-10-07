<!-- markdownlint-disable MD013 -->
# AGENTS.md — security/

OpenVEX v0.2.0 statements for third-party advisories project is not
affected by. Triage process: [docs/development/dependency-advisories.md](../docs/development/dependency-advisories.md).

## Rebase-sensitive invariants

- **`vex/torch.openvex.json`** (ADR-1886): one `not_affected` statement per
  torch advisory for two training packages (`vmaf-train`,
  `vmaf-ensemble-training-kit`). Valid only while
  `scripts/ci/check-torch-scope.py` keeps torch out of every other package.
  Revisit on torch fix release or when training code calls affected
  function.
- **`vex/go.openvex.json`** (ADR-1899): read by `scripts/ci/govulncheck-gate.py`
  (Go CI, `make govulncheck`). govulncheck `-scan symbol -format json`, one
  verdict per advisory on its deepest finding: called symbol fails; package /
  module finding needs `not_affected` statement here;
  `vulnerable_code_not_present` / `component_not_present` cover module level
  only. govulncheck non-zero or no `config` message -> exit 2, never pass.
  JSON mode exits 0 with findings: never trust its exit code for verdicts.
- **Every document** passes `scripts/ci/tests/test_openvex_documents.py`
  (spec enums, `impact_statement` for `not_affected`, `action_statement` for
  `affected`, purl products, one statement per advisory). Edit by hand;
  `vexctl merge <file>` must parse it.
- Gate tests use stand-in binary (`GOVULNCHECK`); real-tool proof
  (`golang.org/x/text` v0.3.7 `ParseAcceptLanguage` -> GO-2022-1059 fail)
  lives in triage page.
