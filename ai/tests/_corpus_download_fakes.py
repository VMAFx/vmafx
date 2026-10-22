# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared fake ``curl`` runner for the corpus download tests."""

from __future__ import annotations

import subprocess
from collections.abc import Collection, Sequence
from pathlib import Path


def fake_curl(
    cmd: Sequence[str],
    attempts: list[str],
    failures: Collection[str],
) -> subprocess.CompletedProcess[str]:
    """Simulate one ``curl --output <path>`` download for corpus ingestion tests.

    Records the target clip name (the ``--output`` basename without ``.part``)
    in ``attempts``. A name in ``failures`` returns exit code 22 with
    ``HTTP 404 Not Found``; any other name gets the payload
    ``downloaded:<name>`` written to the output path and exit code 0.
    """
    output_path: str | None = None
    for i, tok in enumerate(cmd):
        if tok == "--output" and i + 1 < len(cmd):
            output_path = cmd[i + 1]
            break
    assert output_path is not None
    target_name = Path(output_path).name.removesuffix(".part")
    attempts.append(target_name)
    if target_name in failures:
        return subprocess.CompletedProcess(
            args=cmd, returncode=22, stdout="", stderr="HTTP 404 Not Found"
        )
    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(f"downloaded:{target_name}".encode("utf-8"))
    return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")
