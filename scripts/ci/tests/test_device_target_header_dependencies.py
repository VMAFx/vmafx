#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""CI wrapper for device target header dependency regression contract (ADR-1320)."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]
TEST_FILE = ROOT / "core" / "test" / "test_device_target_header_dependencies.py"


def _load_contract_module() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "core_test_device_target_header_dependencies", TEST_FILE
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {TEST_FILE}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


_mod = _load_contract_module()
DeviceTargetHeaderDependencyContractTest = (
    _mod.DeviceTargetHeaderDependencyContractTest  # type: ignore[attr-defined]
)

__all__ = ["DeviceTargetHeaderDependencyContractTest"]

if __name__ == "__main__":
    unittest.main()
