# Refactor: route strict-JSON helpers through `vmaftune.jsonio` (ADR-0988)

Removed private `_nan_to_none` / `_portable_json_dump` / `_emit_json` copies
from `vmaftune.compare`, `vmaftune.report`, and `vmaftune.benchmark`.  All
three now import `dumps_strict` from the canonical `vmaftune.jsonio` module.

Added a small `_nan_to_none` / `_dumps_strict` helper pair to `vmaf_mcp.server`
so the MCP server's final tool-result serialization is RFC 8259-clean even when
a backend emits `NaN` metrics.  The `import math` in `_pick_worst_frames` is
promoted to module scope as a side effect.
