# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Strict JSON tests for model registry helpers."""

from __future__ import annotations

import json
from pathlib import Path

from vmaf_train.registry import dumps_registry_json, write_registry_json


def test_dumps_registry_json_replaces_nonfinite_floats() -> None:
    text = dumps_registry_json(
        {
            "models": [
                {
                    "id": "fixture",
                    "metrics": {
                        "plcc": float("nan"),
                        "srocc": float("inf"),
                        "rmse": 0.25,
                    },
                }
            ]
        }
    )

    assert "NaN" not in text
    assert "Infinity" not in text
    payload = json.loads(text)
    metrics = payload["models"][0]["metrics"]
    assert metrics["plcc"] is None
    assert metrics["srocc"] is None
    assert metrics["rmse"] == 0.25


def test_write_registry_json_is_newline_terminated(tmp_path: Path) -> None:
    path = tmp_path / "registry.json"

    write_registry_json(path, {"models": [{"id": "fixture"}]})

    assert path.read_text(encoding="utf-8").endswith("\n")
