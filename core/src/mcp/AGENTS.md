<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/mcp

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

Orientation for agents working on embedded MCP server. Parent rules apply in full: [../../AGENTS.md](../../AGENTS.md).

- Wholly-new fork files carry `Copyright 2026 Lusoris` ([ADR-0861](../../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)).
- Audit-first validation ([ADR-0209](../../../docs/adr/0209-mcp-embedded-scaffold.md)): every public entry point validates arguments first (`-EINVAL` on NULL pointers, negative fds, NULL paths) before live dispatch or disabled-build `-ENOSYS` fallback.
- Power-of-10 bounds: zero heap allocation on measurement-thread hot path (rule 3); static scalar upper bound on every drain and protocol loop (rule 2).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `/core/src/pdjson.c`, `3rdparty/cJSON/cJSON.c`, `compute_vmaf.c`, `/core/test/test_mcp_smoke.c` | [cjson-pdjson-and-allowlist](AGENTS.d/cjson-pdjson-and-allowlist.md) | PDJSON_STACK_MAX=512, banned-function-free cJSON Semgrep guard, smoke tests, and path allowlist parity. |
| `mcp.c`, `mcp_internal.h`, `meson.build`, `/core/include/libvmaf/libvmaf_mcp.h`, `/core/test/test_mcp_smoke.c` | [scope-status-and-scaffold](AGENTS.d/scope-status-and-scaffold.md) | Embedded MCP scope, live transport status, early -EINVAL validation, and enable_mcp build flags. |
| `*.c`, `*.h`, `meson.build`, `/core/include/libvmaf/libvmaf_mcp.h` | [transport-and-abi-invariants](AGENTS.d/transport-and-abi-invariants.md) | UDS 0700 mode, ephemeral compute_vmaf context, cJSON delta, POSIX-socket loopback SSE, and EINTR retries. |
