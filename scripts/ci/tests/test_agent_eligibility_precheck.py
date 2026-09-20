# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the agent eligibility pre-dispatch gate."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "agent-eligibility-precheck.py"


def load_module():
    """Load the hyphenated executable as a test module."""
    spec = importlib.util.spec_from_file_location("agent_eligibility_precheck", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


gate = load_module()


class PathGlobTests(unittest.TestCase):
    def test_absolute_multi_component_glob_and_literal_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="eligibility-glob-") as temporary:
            root = Path(temporary)
            first = root / "claude-1000" / "run-a" / "tasks" / "first.output"
            second = root / "claude-1000" / "run-b" / "tasks" / "second.output"
            for path in (first, second):
                path.parent.mkdir(parents=True)
                path.write_text("task", encoding="utf-8")

            pattern = str(root / "claude-*" / "*" / "tasks" / "*.output")
            self.assertEqual(set(gate._expand_path_glob(pattern)), {first, second})
            self.assertEqual(tuple(gate._expand_path_glob(str(first))), (first,))
            self.assertEqual(tuple(gate._expand_path_glob(str(root / "missing"))), ())

    def test_active_agent_scan_uses_expanded_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="eligibility-active-") as temporary:
            task = Path(temporary) / "run" / "tasks" / "scope.output"
            task.parent.mkdir(parents=True)
            task.write_text("working on T7-5 now", encoding="utf-8")
            pattern = str(Path(temporary) / "*" / "tasks" / "*.output")
            self.assertFalse(
                gate.check_no_active_agent("T7-5", tasks_glob=pattern, open_branches=[])
            )
            self.assertTrue(
                gate.check_no_active_agent("T3-9", tasks_glob=pattern, open_branches=[])
            )


if __name__ == "__main__":
    unittest.main()
