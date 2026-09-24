"""Contract tests for the repository's Zed 1.18 project configuration.

Zed parses ``.zed/settings.json`` as ``ProjectSettingsContent``, not as the
user-settings schema.  Keep this test narrow: it protects the project-owned
workflow from another silent replacement without pretending to validate a
developer's global Zed preferences.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ZED_DIR = ROOT / ".zed"


def _load(name: str) -> object:
    return json.loads((ZED_DIR / name).read_text(encoding="utf-8"))


def test_settings_are_project_scoped() -> None:
    settings = _load("settings.json")
    assert isinstance(settings, dict)

    # Exact Zed 1.18.1 source: ProjectSettingsContent does not contain these
    # user-only roots.  They would be ignored in .zed/settings.json.
    user_only = {
        "agent",
        "agent_servers",
        "auto_install_extensions",
        "buffer_font_size",
        "telemetry",
        "ui_font_size",
    }
    assert user_only.isdisjoint(settings)

    expected_roots = {
        "context_servers",
        "edit_predictions",
        "ensure_final_newline_on_save",
        "file_scan_exclusions",
        "file_types",
        "format_on_save",
        "hard_tabs",
        "languages",
        "line_ending",
        "lsp",
        "preferred_line_length",
        "remove_trailing_whitespace_on_save",
        "show_wrap_guides",
        "soft_wrap",
        "tab_size",
    }
    assert set(settings) == expected_roots
    assert settings["line_ending"] == "enforce_lf"
    assert settings["soft_wrap"] == "bounded"
    assert "..." in settings["file_scan_exclusions"]


def test_settings_use_current_project_integrations() -> None:
    settings = _load("settings.json")
    assert isinstance(settings, dict)

    assert settings["context_servers"] == {
        "vmafx-mcp": {
            "command": "docker",
            "args": ["exec", "-i", "vmaf-dev-mcp", "vmafx-mcp"],
        }
    }

    disabled_globs = set(settings["edit_predictions"]["disabled_globs"])
    assert {
        ".corpus/**",
        ".workingdir/**",
        "compat/python-vmaf/resource/**",
        "model/**",
        "python/test/resource/**",
        "testdata/**",
        "**/*.onnx",
        "**/*.pkl",
        "**/*.yuv",
    } <= disabled_globs
    assert all(re.search(r"\.workingdir[0-9]+", glob) is None for glob in disabled_globs)

    assert settings["file_types"] == {
        "C++": ["*.cu", "*.cuh", "*.hip", "*.metal", "*.mm"],
        "Meson": ["meson.build", "meson_options.txt"],
    }
    clangd = settings["lsp"]["clangd"]["binary"]
    assert set(clangd) == {"arguments"}
    assert "--compile-commands-dir=build" in clangd["arguments"]
    assert "--clang-tidy" in clangd["arguments"]
    assert not any(arg.startswith("--clang-tidy-checks=") for arg in clangd["arguments"])


def test_tasks_keep_governance_and_use_live_entrypoints() -> None:
    tasks = _load("tasks.json")
    assert isinstance(tasks, list)
    by_label = {task["label"]: task for task in tasks}
    assert len(by_label) == len(tasks)

    assert by_label["Standards: Verify All"]["command"] == "make"
    assert by_label["Standards: Verify All"]["args"] == ["verify-all"]
    assert by_label["Standards: Audit"]["command"] == "standardsctl"
    assert by_label["Standards: Audit"]["args"] == ["audit"]
    assert by_label["Standards: Compile Context"]["args"] == [
        "compile-context",
        "--verify",
    ]

    assert by_label["Dev container: build exact source"]["command"] == (
        "dev/scripts/container-build.sh"
    )
    assert by_label["Dev container: start"]["args"][-3:] == ["up", "-d", "dev-mcp"]
    assert by_label["MCP: probe running container"]["command"] == ("dev/scripts/dev-mcp-probe.sh")

    tune = by_label["vmaf-tune: compare CPU smoke"]
    assert tune["command"] == "docker"
    assert "vmaf-tune" in tune["args"]
    assert "compare" in tune["args"]
    assert "--src" in tune["args"]
    assert "--ref" not in tune["args"]
    assert "--dis" not in tune["args"]

    serialized = json.dumps(tasks)
    for stale in (
        ".venv/bin/",
        "scripts/dev/regen_docs.py",
        "scripts/dev/validate_scores.py",
        "tools/vmaf-tune/vmaftune/compare.py",
        'vmaf-mcp"',
    ):
        assert stale not in serialized
    assert re.search(r"\.workingdir[0-9]+", serialized) is None


def test_debug_scenarios_target_current_surfaces() -> None:
    scenarios = _load("debug.json")
    assert isinstance(scenarios, list)
    by_label = {scenario["label"]: scenario for scenario in scenarios}
    assert len(by_label) == len(scenarios)

    assert by_label["Debug: test_feature (ASan/UBSan build)"]["adapter"] == "CodeLLDB"
    assert by_label["Debug: pytest current file"]["adapter"] == "Debugpy"
    assert by_label["Debug: vmafx-mcp (stdio)"] == {
        "adapter": "Delve",
        "label": "Debug: vmafx-mcp (stdio)",
        "request": "launch",
        "mode": "debug",
        "program": "./cmd/vmafx-mcp",
        "cwd": "$ZED_WORKTREE_ROOT",
    }

    tune = by_label["Debug: vmaf-tune compare"]
    assert tune["adapter"] == "Debugpy"
    assert tune["module"] == "vmaftune.cli"
    assert tune["env"]["PYTHONPATH"] == "$ZED_WORKTREE_ROOT/tools/vmaf-tune/src"

    serialized = json.dumps(scenarios)
    assert "vmaf_mcp.server" not in serialized
    assert "vmaftune.compare" not in serialized


def test_current_guide_owns_the_contract() -> None:
    guide = (ROOT / "docs/development/ide-setup.md").read_text(encoding="utf-8")
    archive = (ROOT / "docs/development/zed-migration-plan-2026-05-22.md").read_text(
        encoding="utf-8"
    )
    old_research = (ROOT / "docs/research/0729-zed-config-1-3-6-refresh.md").read_text(
        encoding="utf-8"
    )

    assert "Zed 1.18.1" in guide
    assert "Project settings cannot install ACP agents" in guide
    assert "docker exec -i vmaf-dev-mcp vmafx-mcp" in guide
    assert "Historical snapshot" in archive
    assert "current reference" not in archive
    assert "Historical evidence, not current configuration guidance" in old_research
