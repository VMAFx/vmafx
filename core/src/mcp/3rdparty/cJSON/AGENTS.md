<!-- markdownlint-disable MD013 -->
# AGENTS.md — vendored cJSON

Parent: [../../AGENTS.md](../../AGENTS.md) (core/src/mcp/).

## Vendor policy

Dir = vendored copy of [cJSON](https://github.com/DaveGamble/cJSON), pinned
**v1.7.18** (upstream stable release, 2026-05-16).

- **Do not** apply NOLINTs to banned-function violations here. Fix call site,
  or sync to clean upstream version with them addressed instead.
- **Banned functions** (`sprintf`, `strcpy`, `strcat`, `strtok`, `atoi`, `atof`,
  `gets`, `rand`, `system`): not exempt from fork's lint rules, vendored or
  not. See `docs/principles.md` §1.2 rule 30 and
  [ADR-0683](../../../../docs/adr/0683-cjson-banned-function-remediation.md).
- **To update**: replace `cJSON.c` and `cJSON.h` with upstream release. Verify
  no banned functions remain, run:
  `grep -n '\bsprintf\b\|\bstrcpy\b\|\bstrcat\b' core/src/mcp/3rdparty/cJSON/cJSON.c`
  after sync. Re-apply ADR-0683 banned-function fixes if regressed.
- `LICENSE` file: keep in sync with upstream release.

## Rebase note

cJSON = internal dependency of MCP server (`core/src/mcp/`). Not in public C
API (`core/include/`); not consumed by `ffmpeg-patches/`. Upstream Netflix/vmaf
does not vendor cJSON -> no rebase conflict risk from Netflix side. Conflict
risk only if fork adds second cJSON copy elsewhere.
