# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``vmaf-mcp`` — MCP server exposing VMAF scoring to LLM tooling.

Sub-modules:

* :mod:`vmaf_mcp.server`         — JSON-RPC MCP server (tool registry,
  handlers, stdio + Unix-socket transports).
* :mod:`vmaf_mcp.http_transport` — optional HTTP transport adapter.

The package surface is a namespace per ADR-0911; downstream callers
should import from sub-modules directly.
"""

__version__ = "0.1.0"

__all__ = ["__version__"]
