<!-- markdownlint-disable MD013 -->
# AGENTS.md — vendored cJSON

Parent: [../../AGENTS.md](../../AGENTS.md) (core/src/mcp/).

## Vendor policy

This directory contains a vendored copy of [cJSON](https://github.com/DaveGamble/cJSON)
pinned at **v1.7.18** (the upstream stable release as of 2026-05-16).

- **Do not** apply NOLINTs to banned-function violations in this file. Instead,
  either fix the call site or sync to a clean upstream version that has addressed them.
- **Banned functions** (`sprintf`, `strcpy`, `strcat`, `strtok`, `atoi`, `atof`,
  `gets`, `rand`, `system`) are not exempt from the fork's lint rules, even in vendored
  code. See `docs/principles.md` §1.2 rule 30 and
  [ADR-0683](../../../../docs/adr/0683-cjson-banned-function-remediation.md).
- **To update**: replace `cJSON.c` and `cJSON.h` with the upstream release, then
  verify that no banned functions remain by running:
  `grep -n '\bsprintf\b\|\bstrcpy\b\|\bstrcat\b' core/src/mcp/3rdparty/cJSON/cJSON.c`
  after any sync. Re-apply the banned-function fixes documented in ADR-0683 if they
  regress.
- The `LICENSE` file must be kept in sync with the upstream release.

## Rebase note

cJSON is an internal dependency of the MCP server (`core/src/mcp/`). It does not
appear in the public C API (`core/include/`) and is not consumed by
`ffmpeg-patches/`. Upstream Netflix/vmaf does not vendor cJSON, so there is no rebase
conflict risk from the Netflix side. Conflicts can only arise if this fork adds a
second copy of cJSON elsewhere.
