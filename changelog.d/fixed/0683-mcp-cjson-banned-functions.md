- Replace `sprintf`/`strcpy` calls with `snprintf`/`memcpy` in vendored MCP cJSON
  (`core/src/mcp/3rdparty/cJSON/cJSON.c`) to satisfy the fork's banned-function
  rule (`docs/principles.md` §1.2 r30 / ADR-0141). Adds vendor-policy `AGENTS.md`
  with update instructions (ADR-0683).
