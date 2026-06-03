# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the U2NetP mirror exporter."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

torch = pytest.importorskip("torch")
onnx = pytest.importorskip("onnx")

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

import export_u2netp_mirror as exporter  # noqa: E402


def _write_fake_upstream(root: Path) -> Path:
    module_path = root / "model" / "u2net.py"
    module_path.parent.mkdir(parents=True)
    module_path.write_text(
        """
import torch
from torch import nn


class U2NETP(nn.Module):
    def __init__(self, in_ch=3, out_ch=1):
        super().__init__()
        self.conv = nn.Conv2d(in_ch, out_ch, kernel_size=1)

    def forward(self, x):
        y = torch.sigmoid(self.conv(x))
        return y, y, y, y, y, y, y
""".lstrip(),
        encoding="utf-8",
    )
    (root / "LICENSE").write_text(
        "Apache License\nVersion 2.0, January 2004\n",
        encoding="utf-8",
    )
    return module_path


def _write_fake_checkpoint(path: Path) -> None:
    state = {
        "module.conv.weight": torch.ones(1, 3, 1, 1, dtype=torch.float32),
        "module.conv.bias": torch.zeros(1, dtype=torch.float32),
    }
    torch.save(state, path)


def test_export_u2netp_mirror_writes_contract_and_manifest(tmp_path: Path) -> None:
    upstream = tmp_path / "upstream"
    _write_fake_upstream(upstream)
    checkpoint = upstream / "u2netp.pth"
    _write_fake_checkpoint(checkpoint)

    output = tmp_path / "u2netp_mirror.onnx"
    manifest = tmp_path / "u2netp_mirror.manifest.json"

    exporter.main(
        [
            "--upstream-dir",
            str(upstream),
            "--checkpoint",
            str(checkpoint),
            "--output",
            str(output),
            "--manifest-out",
            str(manifest),
            "--height",
            "8",
            "--width",
            "8",
        ]
    )

    graph = onnx.load(output)
    assert graph.graph.input[0].name == "input"
    assert graph.graph.output[0].name == "saliency_map"
    metadata = {prop.key: prop.value for prop in graph.metadata_props}
    assert metadata["lusoris.model_id"] == "u2netp_mirror_v1"
    assert metadata["lusoris.upstream_checkpoint_sha256"] == exporter.sha256(checkpoint)

    payload = json.loads(manifest.read_text(encoding="utf-8"))
    assert payload["schema"] == "u2netp-mirror-export-manifest-v1"
    assert payload["model_id"] == "u2netp_mirror_v1"
    assert payload["upstream"]["checkpoint_sha256"] == exporter.sha256(checkpoint)
    assert payload["upstream"]["license_sha256"] == exporter.sha256(upstream / "LICENSE")
    assert payload["upstream"]["notice_present"] is False
    assert payload["export"]["output_name"] == "saliency_map"
    assert payload["export"]["selected_upstream_output"] == "d0"
    assert payload["outputs"]["onnx_sha256"] == exporter.sha256(output)
    assert payload["run_provenance"]["entrypoint"]["path"] == ("ai/scripts/export_u2netp_mirror.py")


def test_export_u2netp_mirror_requires_apache_license(tmp_path: Path) -> None:
    upstream = tmp_path / "upstream"
    _write_fake_upstream(upstream)
    (upstream / "LICENSE").write_text("Custom license\n", encoding="utf-8")
    checkpoint = upstream / "u2netp.pth"
    _write_fake_checkpoint(checkpoint)

    with pytest.raises(RuntimeError, match=r"Apache-2\.0"):
        exporter.main(
            [
                "--upstream-dir",
                str(upstream),
                "--checkpoint",
                str(checkpoint),
                "--output",
                str(tmp_path / "out.onnx"),
            ]
        )
