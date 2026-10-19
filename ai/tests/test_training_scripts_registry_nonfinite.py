# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FR regressor trainers write ``model/tiny/registry.json`` as strict JSON.

``T-BUG048-AI-SCRIPT-HELPERS-2026-09-26`` (registry half): the four trainers
wrote the registry with a raw ``json.dumps``, which prints ``NaN`` for a
non-finite value and produces a file no strict reader accepts. They now go
through :func:`vmaf_train.registry.write_registry_json`. Positive: a registry
row that already holds a non-finite number comes back as ``null``. Negative: no
``NaN`` / ``Infinity`` token survives. Boundary: a registry of finite values is
byte-identical to the old ``json.dumps(..., indent=2, sort_keys=True)`` output.
"""

from __future__ import annotations

import json
import sys
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

# pylint: disable=wrong-import-position
import train_fr_regressor as v1  # noqa: E402
import train_fr_regressor_v2 as v2  # noqa: E402
import train_fr_regressor_v2_ensemble as ensemble  # noqa: E402
import train_fr_regressor_v3 as v3  # noqa: E402

_SEED_ROW = '{"id": "seed_row", "kind": "fr", "onnx": "seed.onnx", "plcc": %s}'


def _reject(token: str) -> object:
    raise AssertionError(f"registry.json holds the non-standard token {token}")


def _upsert_v1(path: Path) -> None:
    v1._upsert_registry_entry(path, Path("fr_regressor_v1.onnx"), "0" * 64, "notes")


def _upsert_v2(path: Path) -> None:
    v2._update_fr_v2_registry(path, "fr_regressor_v2.onnx", "0" * 64, "notes", True)


def _upsert_ensemble(path: Path) -> None:
    member = {"id": "ens_seed0", "onnx": "ens_seed0.onnx", "sha256": "0" * 64, "seed": 0}
    ensemble._update_registry(path, ensemble_id="ens", members=[member], smoke=True)


def _upsert_v3(path: Path) -> None:
    v3._upsert_v3_registry_row(path, {"id": "fr_regressor_v3", "kind": "fr"})


_UPSERTS: dict[str, Callable[[Path], None]] = {
    "v1": _upsert_v1,
    "v2": _upsert_v2,
    "ensemble": _upsert_ensemble,
    "v3": _upsert_v3,
}


def test_registry_holding_a_non_finite_value_is_rewritten_as_strict_json(
    tmp_path: Path,
) -> None:
    for name, upsert in _UPSERTS.items():
        for token in ("NaN", "Infinity", "-Infinity"):
            registry = tmp_path / f"{name}-{token}.json"
            registry.write_text('{"models": [' + _SEED_ROW % token + "]}\n", encoding="utf-8")

            upsert(registry)

            text = registry.read_text(encoding="utf-8")
            rows = json.loads(text, parse_constant=_reject)["models"]
            seed = next(row for row in rows if row["id"] == "seed_row")
            assert seed["plcc"] is None, (name, token)
            assert text.endswith("\n"), (name, token)


def test_finite_registry_keeps_the_previous_byte_layout(tmp_path: Path) -> None:
    for name, upsert in _UPSERTS.items():
        registry = tmp_path / f"{name}.json"
        registry.write_text('{"models": [' + _SEED_ROW % "0.5" + "]}\n", encoding="utf-8")

        upsert(registry)

        text = registry.read_text(encoding="utf-8")
        assert text == json.dumps(json.loads(text), indent=2, sort_keys=True) + "\n", name
