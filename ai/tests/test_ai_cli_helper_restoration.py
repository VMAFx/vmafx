# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Guard the shared bootstrap and CLI contract restored for BUG-048."""

from __future__ import annotations

import ast
from pathlib import Path
from typing import Any, Callable, TypeVar, cast

import pytest

F = TypeVar("F", bound=Callable[..., Any])


def _parametrize(*args: Any, **kwargs: Any) -> Callable[[F], F]:
    return cast(Callable[[F], F], pytest.mark.parametrize(*args, **kwargs))


_SCRIPTS = (
    "eval_loso_3arch.py",
    "eval_loso_mlp_small.py",
    "eval_loso_vmaf_tiny_v3.py",
    "eval_loso_vmaf_tiny_v4.py",
    "eval_loso_vmaf_tiny_v5.py",
    "eval_multiseed_v3_v4.py",
    "export_tiny_models.py",
    "measure_quant_drop.py",
    "measure_quant_drop_per_ep.py",
    "ptq_dynamic.py",
    "ptq_static.py",
    "qat_train.py",
)
_SCRIPT_DIR = Path(__file__).resolve().parents[1] / "scripts"


def _call_name(node: ast.Call) -> str:
    if isinstance(node.func, ast.Name):
        return node.func.id
    if isinstance(node.func, ast.Attribute):
        parts = [node.func.attr]
        value = node.func.value
        while isinstance(value, ast.Attribute):
            parts.append(value.attr)
            value = value.value
        if isinstance(value, ast.Name):
            parts.append(value.id)
        return ".".join(reversed(parts))
    return ""


def _find_main(tree: ast.Module, script_name: str) -> ast.FunctionDef | ast.AsyncFunctionDef:
    main = next(
        (
            node
            for node in tree.body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == "main"
        ),
        None,
    )
    assert main is not None, f"{script_name} has no main() seam"
    return main


@_parametrize("script_name", _SCRIPTS)
def test_ai_cli_uses_shared_bootstrap_and_parser_contract(script_name: str) -> None:
    """Each restored CLI delegates imports, parsing, and ``None`` argv handling."""
    script = _SCRIPT_DIR / script_name
    tree = ast.parse(script.read_text(encoding="utf-8"), filename=str(script))
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)]
    call_names = [_call_name(call) for call in calls]

    bootstrap_calls = [call for call in calls if _call_name(call) == "bootstrap_ai_script"]
    assert len(bootstrap_calls) == 1, f"{script_name} hand-rolls import bootstrap"
    assert "sys.path.insert" not in call_names, f"{script_name} mutates sys.path directly"
    assert "make_argument_parser" in call_names, f"{script_name} constructs its own parser"
    assert "argparse.ArgumentParser" not in call_names, f"{script_name} bypasses parser helper"

    main = _find_main(tree, script_name)
    assert (
        main.args.args and main.args.args[0].arg == "argv"
    ), f"{script_name} main() cannot accept an explicit replay argv"
    assert main.args.defaults and isinstance(
        main.args.defaults[-1], ast.Constant
    ), f"{script_name} main() does not default argv to None"
    assert main.args.defaults[-1].value is None

    main_calls = [node for node in ast.walk(main) if isinstance(node, ast.Call)]
    collect_calls = [call for call in main_calls if _call_name(call) == "collect_cli_argv"]
    assert len(collect_calls) == 1, f"{script_name} bypasses shared None-argv normalization"
    assert len(collect_calls[0].args) == 1
    assert isinstance(collect_calls[0].args[0], ast.Name)
    assert collect_calls[0].args[0].id == "argv"

    sys_argv_reads = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Attribute)
        and isinstance(node.value, ast.Name)
        and node.value.id == "sys"
        and node.attr == "argv"
    ]
    assert not sys_argv_reads, f"{script_name} reads sys.argv outside the shared helper"

    imports_repo_package = any(
        isinstance(node, ast.ImportFrom)
        and (node.module or "").startswith("ai.")
        and node.module != "ai.scripts._script_bootstrap"
        for node in ast.walk(tree)
    )
    if imports_repo_package:
        repo_root_flags = [
            keyword for keyword in bootstrap_calls[0].keywords if keyword.arg == "include_repo_root"
        ]
        assert len(repo_root_flags) == 1
        assert isinstance(repo_root_flags[0].value, ast.Constant)
        assert (
            repo_root_flags[0].value.value is True
        ), f"{script_name} imports the ai package but omits its root from direct invocations"
