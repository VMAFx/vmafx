- **docs(mcp)**: `core/src/mcp/AGENTS.md` and the
  `core/src/mcp/mcp_internal.h` doc-comment header refreshed to the
  current live v3 state. The earlier text still described the subtree as
  the audit-first T5-2a stub ("stdio runtime only; SSE/UDS still return
  `-ENOSYS`"), which has been wrong since T5-2b shipped the stdio
  dispatcher and the v2 UDS + v3 loopback-HTTP/SSE transports landed
  (`transport_uds.c` 182 LoC, `transport_sse.c` 533 LoC; mongoose
  rejected on GPL-2.0-only-OR-commercial license incompatibility per the
  2026-05-09 audit). No code changes — text-only correction matching
  what `mcp.c`, `dispatcher.c`, and `compute_vmaf.c` already do.
