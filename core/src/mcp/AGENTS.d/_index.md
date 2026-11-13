<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/mcp

Orientation for agents working on embedded MCP server. Parent rules apply in full: [../../AGENTS.md](../../../AGENTS.md).

- Wholly-new fork files carry `Copyright 2026 Lusoris` ([ADR-0861](../../../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)).
- Audit-first validation ([ADR-0209](../../../../docs/adr/0209-mcp-embedded-scaffold.md)): every public entry point validates arguments first (`-EINVAL` on NULL pointers, negative fds, NULL paths) before live dispatch or disabled-build `-ENOSYS` fallback.
- Power-of-10 bounds: zero heap allocation on measurement-thread hot path (rule 3); static scalar upper bound on every drain and protocol loop (rule 2).
