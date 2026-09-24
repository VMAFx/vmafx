#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep migrated issue references bound to the repository that owned them.

The active ``VMAFx/vmafx`` repository started a new issue/PR number space after
the project moved away from ``lusoris/vmaf``.  A bare ``#NNN`` therefore points
at the active repository even when the surrounding historical record describes
an issue or pull request from the archived repository.

This checker is deliberately context-scoped.  It protects only references whose
origin is established by repository history, and it does not reject bare issue
or pull-request references elsewhere: those normally refer to the active fork.
Contracts use stable prose anchors and Markdown logical blocks instead of line
numbers or exact whitespace, so ordinary reflow does not disable the gate.

Exit: 0 clean, 1 provenance finding, 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


@dataclass(frozen=True)
class ProvenanceContract:
    """One historical prose context and the repository-qualified refs it needs."""

    path: str
    anchor: str
    refs: tuple[str, ...]


CONTRACTS = (
    ProvenanceContract(
        "docs/state.md",
        "wrong kernel parameter type in cuLaunchKernel dispatch helpers",
        ("lusoris/vmaf#857",),
    ),
    ProvenanceContract(
        "docs/state.md",
        "The three kernel dispatch helpers in `integer_cambi_cuda.c`",
        ("lusoris/vmaf#857",),
    ),
    ProvenanceContract(
        "docs/state.md",
        "T-CAMBI-CUDA-HOST-PREPROCESSING-SEGV (Issue",
        ("lusoris/vmaf#857", "lusoris/vmaf#870"),
    ),
    ProvenanceContract(
        "docs/state.md",
        "FFmpeg `libvmaf_vulkan` filter wall-clock serialisation (lawrence profile 2026-04-30)",
        ("lusoris/vmaf#239", "lusoris/vmaf#241", "lusoris/vmaf#310"),
    ),
    ProvenanceContract(
        "docs/adr/0251-vulkan-async-pending-fence.md",
        "lawrence's 2026-04-30 profile",
        ("lusoris/vmaf#239",),
    ),
    ProvenanceContract(
        "docs/adr/0251-vulkan-async-pending-fence.md",
        "| **Stay on v1** |",
        ("lusoris/vmaf#239",),
    ),
    ProvenanceContract(
        "docs/adr/0251-vulkan-async-pending-fence.md",
        "- Profile signal:",
        ("lusoris/vmaf#239",),
    ),
    ProvenanceContract(
        "docs/adr/0464-cambi-cuda-smem-tile.md",
        "measured against the cambi_score baseline",
        ("lusoris/vmaf#857",),
    ),
    ProvenanceContract(
        "docs/research/0042-vulkan-async-pending-fence.md",
        "confirms the predicted bottleneck",
        ("lusoris/vmaf#239",),
    ),
    ProvenanceContract(
        "docs/research/0042-vulkan-async-pending-fence.md",
        "evidence the wait dominates the FFmpeg filter wall-clock",
        ("lusoris/vmaf#239",),
    ),
    ProvenanceContract(
        "docs/research/0042-vulkan-async-pending-fence.md",
        "— profile signal.",
        ("lusoris/vmaf#239",),
    ),
    ProvenanceContract(
        "docs/research/0090-state-md-row-audit-2026-05-09.md",
        "| RC16 |",
        ("lusoris/vmaf#239", "lusoris/vmaf#241"),
    ),
    ProvenanceContract(
        "docs/research/0135-cambi-cuda-smem-tile-2026-05-16.md",
        "segfault window",
        ("lusoris/vmaf#857", "lusoris/vmaf#870"),
    ),
    ProvenanceContract(
        "docs/research/0135-cambi-cuda-smem-tile-2026-05-16.md",
        "cambi_cuda segfault (blocks end-to-end validation)",
        ("lusoris/vmaf#857",),
    ),
    ProvenanceContract(
        "docs/research/0135-cambi-cuda-smem-tile-2026-05-16.md",
        "host-preprocessing fix (prerequisite for end-to-end run)",
        ("lusoris/vmaf#870",),
    ),
)


def logical_blocks(text: str) -> list[str]:
    """Return whitespace-normalised Markdown paragraphs and individual rows."""

    blocks: list[str] = []
    paragraph: list[str] = []

    def flush() -> None:
        if paragraph:
            blocks.append(" ".join(" ".join(paragraph).split()))
            paragraph.clear()

    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            flush()
        elif stripped.startswith("|"):
            flush()
            blocks.append(" ".join(stripped.split()))
        else:
            paragraph.append(stripped)
    flush()
    return blocks


def check_contract(text: str, contract: ProvenanceContract) -> list[str]:
    """Check one semantic context, failing closed when its anchor drifts."""

    matches = [block for block in logical_blocks(text) if contract.anchor in block]
    if len(matches) != 1:
        return [
            f"anchor {contract.anchor!r} matched {len(matches)} logical blocks; expected exactly 1"
        ]

    block = matches[0]
    findings = [f"missing {ref}" for ref in contract.refs if ref not in block]

    numbers = sorted({ref.rsplit("#", 1)[1] for ref in contract.refs})
    bare = re.compile(rf"(?<![A-Za-z0-9_./-])#({'|'.join(numbers)})\b")
    for match in sorted(set(bare.findall(block))):
        findings.append(f"unqualified archived reference #{match}")
    return findings


def check_root(root: Path, contracts: Sequence[ProvenanceContract] = CONTRACTS) -> list[str]:
    """Return repository-relative provenance findings under ``root``."""

    findings: list[str] = []
    texts: dict[str, str | None] = {}
    for contract in contracts:
        if contract.path not in texts:
            path = root / contract.path
            if not path.is_file():
                findings.append(f"{contract.path}: file not found")
                texts[contract.path] = None
            else:
                texts[contract.path] = path.read_text(encoding="utf-8")
        text = texts[contract.path]
        if text is None:
            continue
        for finding in check_contract(text, contract):
            findings.append(f"{contract.path}: {finding}")
    return findings


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: derived from this script)",
    )
    args = parser.parse_args(argv)
    if not args.root.is_dir():
        print(f"check-issue-reference-provenance: root not found: {args.root}", file=sys.stderr)
        return 2

    findings = check_root(args.root)
    if findings:
        print(
            "::error title=historical issue-reference provenance::"
            "repository-qualified references drifted",
            file=sys.stderr,
        )
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1

    print(
        "check-issue-reference-provenance: OK "
        f"({len(CONTRACTS)} historical contexts repository-qualified)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
