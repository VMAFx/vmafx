---
paths:
  - scripts/ci/*source-adr-citations*
  - scripts/ci/tests/test_check_source_adr_citations.py
invariant: Plain `ADR-NNNN` in source binds to exact file, derived each run; registry: `retired` + `fixtures` only.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Source ADR citation provenance (ADR-1311, ADR-2200)

`check-source-adr-citations.py` owns plain `ADR-NNNN` references in tracked
implementation and build/control files. Preserve explicit source-suffix /
basename scope, derived `number -> filename -> path/count` bindings,
hand-governed retirement records, exact-path fixture exemptions,
always-run pre-commit hook, and positive/negative/boundary test suite
together.

Live bindings derive from `git ls-files` and `docs/adr/` on every run
(ADR-2200). Not recorded: change citing ADR edits no shared file.
`source-adr-citations.json` = schema 2, holds only `retired` and `fixtures`;
`live` key = error; no `--write`. Keep refusals: number with no ADR file and
no retirement record; retired number with ADR file again; fixture number cited
outside its recorded paths or with other counts. Never auto-invent
retirement, broaden fixture exemption, or accept reused retired number.
Markdown links remain separate `check-adr-links.py` contract. `mkdocs.yml` is
prose navigation, not source, and stays outside this gate so its full ADR
index cannot masquerade as 1,000+ implementation citations.
Disposable test repositories must strip every inherited `GIT_*` variable,
disable caller system/global configuration, hooks, and signing for both direct
Git calls and checker subprocesses. regression poisons `GIT_INDEX_FILE`,
`GIT_DIR`, `GIT_WORK_TREE`, and `GIT_PREFIX` and byte-checks caller index
and repository configuration; do not weaken it to clean-shell-only test.
