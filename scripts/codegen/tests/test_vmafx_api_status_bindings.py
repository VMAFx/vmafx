# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""One status definition must reach Python, Go and Rust, including drift checks."""

from __future__ import annotations

import ast
import io
import re
import tempfile
import unittest
from contextlib import redirect_stdout
from dataclasses import replace
from pathlib import Path

from support import DEFINITION
from vmafx_api import cli
from vmafx_api.loader import load

PYTHON = "bindings/python/vmafx/_api.py"
GO = "pkg/vmafx/status_gen.go"
RUST = "bindings/rust/vmafx/src/status_gen.rs"
PATHS = (PYTHON, GO, RUST)


def python_pairs(text: str) -> set[tuple[str, int]]:
    status = next(
        node
        for node in ast.parse(text).body
        if isinstance(node, ast.ClassDef) and node.name == "Status"
    )
    return {
        ("VMAFX_" + node.targets[0].id, ast.literal_eval(node.value))
        for node in status.body
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
    }


def go_pairs(text: str) -> set[tuple[str, int]]:
    values = dict(re.findall(r"^const (Status\w+) Status = (-?\d+)$", text, re.MULTILINE))
    names = re.findall(r'case (Status\w+):\s+return "(VMAFX_\w+)"', text)
    return {(name, int(values[constant])) for constant, name in names}


def rust_pairs(text: str) -> set[tuple[str, int]]:
    values = dict(re.findall(r"^    (\w+) = (-?\d+),$", text, re.MULTILINE))
    names = re.findall(r'Self::(\w+) => "(VMAFX_\w+)"', text)
    return {(name, int(values[variant])) for variant, name in names}


class StatusBindingsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = load(DEFINITION)
        rendered = cli.render(self.api)
        self.files = {path: rendered[path] for path in PATHS}

    def test_all_three_tables_match_definition(self) -> None:
        expected = {(status.name, status.value) for status in self.api.statuses}
        readers = {PYTHON: python_pairs, GO: go_pairs, RUST: rust_pairs}
        for path, reader in readers.items():
            with self.subTest(path=path):
                self.assertEqual(reader(self.files[path]), expected)

    def test_registered_outputs_are_current(self) -> None:
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()):
            root = Path(directory)
            self.assertEqual(cli.write(root, self.files), 0)
            self.assertEqual(cli.check(root, self.files), 0)

    def test_rust_variant_docs_preserve_definition_text(self) -> None:
        enum = self.files[RUST].split("pub enum Status {\n", 1)[1].split("\n}", 1)[0]
        docs = []
        comment = []
        for line in enum.splitlines():
            if line.strip().startswith("/// "):
                comment.append(line.strip().removeprefix("/// "))
            elif " = " in line:
                docs.append(" ".join(comment).replace("`", ""))
                comment = []
        self.assertEqual(docs, [status.doc.replace("`", "") for status in self.api.statuses])

    def test_planted_value_change_reports_drift_in_every_language(self) -> None:
        altered = replace(self.api.statuses[-1], value=-99)
        changed_api = replace(self.api, statuses=(*self.api.statuses[:-1], altered))
        changed = cli.render(changed_api)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with redirect_stdout(io.StringIO()):
                cli.write(root, self.files)
            for path in PATHS:
                with self.subTest(path=path):
                    self.assertNotEqual(self.files[path], changed[path])
                    out = io.StringIO()
                    with redirect_stdout(out):
                        result = cli.check(root, {path: changed[path]})
                    self.assertEqual(result, 1)
                    self.assertIn(path, out.getvalue())
                    self.assertIn("--write", out.getvalue())


if __name__ == "__main__":
    unittest.main()
