# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared scoring interface between the MCP server and HTTP adapter.

The stdio server owns scoring policy and implementation.  The HTTP transport
depends only on this narrow interface, keeping the production dependency graph
one-way and preventing the HTTP adapter from importing the stdio server.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Protocol


class HttpScoringRuntime(Protocol):
    """Operations the HTTP transport needs from the scoring implementation."""

    def vmaf_binary(self) -> Path:
        """Return the configured VMAF executable path."""

    def build_request(
        self,
        *,
        reference: Any,
        distorted: Any,
        width: Any,
        height: Any,
        pixfmt: Any,
        bitdepth: Any,
        model: Any,
        backend: Any,
        precision: Any,
    ) -> Any:
        """Validate HTTP fields and construct the canonical score request."""

    async def run_score(self, request: Any) -> dict[str, Any]:
        """Execute one canonical score request."""

    def dumps_strict(self, data: Any) -> str:
        """Serialize a response as RFC 8259 JSON."""


_runtime: HttpScoringRuntime | None = None


def install_http_scoring_runtime(runtime: HttpScoringRuntime) -> None:
    """Install the process-wide scoring adapter used by the HTTP transport.

    Canonical server import is intentionally non-destructive so an embedding
    process can install its own adapter before importing :mod:`vmaf_mcp.server`.
    Per-server adapters are passed explicitly and never replace this registry.
    """
    global _runtime
    if _runtime is not None:
        return
    _runtime = runtime


def get_http_scoring_runtime() -> HttpScoringRuntime:
    """Return the installed scoring adapter or fail with an actionable error."""
    if _runtime is None:
        raise RuntimeError(
            "HTTP scoring runtime is not installed; start HTTP through vmaf_mcp.server:main"
        )
    return _runtime
