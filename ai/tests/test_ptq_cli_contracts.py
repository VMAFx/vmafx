# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression coverage for PTQ and quantization measurement CLI contracts.

Ensures ptq_dynamic.py, ptq_static.py, and measure_quant_drop.py fulfill the
shared CLI-helper, help, import isolation, and argument replay contracts.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path
from types import ModuleType
from typing import Any, Callable, TypeVar, cast

import pytest

F = TypeVar("F", bound=Callable[..., Any])


def _parametrize(*args: Any, **kwargs: Any) -> Callable[[F], F]:
    return cast(Callable[[F], F], pytest.mark.parametrize(*args, **kwargs))


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "ai" / "scripts"

_TARGET_SCRIPTS = (
    "ptq_dynamic.py",
    "ptq_static.py",
    "measure_quant_drop.py",
)


def _run_help(script: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(script), "--help"],
        check=False,
        capture_output=True,
        text=True,
        timeout=15,
    )


def _load_script(
    script_name: str,
    suffix: str,
    monkeypatch: pytest.MonkeyPatch,
) -> ModuleType:
    """Load a script with the Python 3.14-safe importlib contract."""
    spec = importlib.util.spec_from_file_location(
        f"{script_name}_{suffix}", SCRIPTS_DIR / script_name
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, mod)
    spec.loader.exec_module(mod)
    return mod


@_parametrize("script_name", _TARGET_SCRIPTS)
def test_cli_help_contract(script_name: str) -> None:
    """Entry-point must return 0 and surface meaningful CLI help."""
    cp = _run_help(SCRIPTS_DIR / script_name)
    assert cp.returncode == 0, cp.stderr
    assert "--help" in cp.stdout or "usage:" in cp.stdout.lower()


@_parametrize("script_name", _TARGET_SCRIPTS)
def test_cli_import_isolation_contract(script_name: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """Import the module without executing main() — must not require heavy deps."""
    mod = _load_script(script_name, "isolation_test", monkeypatch)
    assert callable(mod.main)


@_parametrize("script_name", _TARGET_SCRIPTS)
def test_cli_argument_contract_explicit_help(
    script_name: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    """main() must accept an explicit list of CLI argument strings."""
    mod = _load_script(script_name, "explicit_argv_test", monkeypatch)
    with pytest.raises(SystemExit) as exc_info:
        mod.main(["--help"])
    assert exc_info.value.code == 0


@_parametrize("script_name", _TARGET_SCRIPTS)
def test_cli_argument_contract_none_argv(script_name: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """main(None) must collect arguments from sys.argv via collect_cli_argv."""
    mod = _load_script(script_name, "none_argv_test", monkeypatch)
    monkeypatch.setattr(sys, "argv", [script_name, "--help"])
    with pytest.raises(SystemExit) as exc_info:
        mod.main(None)
    assert exc_info.value.code == 0
