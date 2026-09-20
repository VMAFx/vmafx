#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for the optional NEO GitHub build-secret contract."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "scripts/ci/check-dev-container-build-secret.py"
SPEC = importlib.util.spec_from_file_location("check_dev_container_build_secret", CHECKER)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BuildSecretContract(unittest.TestCase):
    def setUp(self) -> None:
        self.containerfile = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")
        self.compose = (ROOT / "dev/docker-compose.yml").read_text(encoding="utf-8")
        self.workflow = (ROOT / ".github/workflows/dev-container-build.yml").read_text()
        self.docs = (ROOT / "docs/development/dev-mcp.md").read_text(encoding="utf-8")
        self.fetcher = (ROOT / "dev/scripts/fetch-intel-neo.py").read_text(encoding="utf-8")

    def errors(
        self,
        *,
        containerfile: str | None = None,
        compose: str | None = None,
        workflow: str | None = None,
        docs: str | None = None,
        fetcher: str | None = None,
    ) -> list[str]:
        errors = MODULE.validate_containerfile(containerfile or self.containerfile)
        errors.extend(MODULE.validate_compose(compose or self.compose))
        errors.extend(
            MODULE.validate_callers(
                workflow or self.workflow,
                docs or self.docs,
                fetcher or self.fetcher,
            )
        )
        return errors

    def test_current_tree_passes(self) -> None:
        self.assertEqual(self.errors(), [])

    def test_arg_and_env_token_declarations_fail(self) -> None:
        for declaration in ('ARG GITHUB_TOKEN=""', "ENV GITHUB_TOKEN=secret"):
            with self.subTest(declaration=declaration):
                text = self.containerfile + f"\n{declaration}\n"
                self.assertTrue(
                    any("ARG or ENV" in error for error in self.errors(containerfile=text))
                )

    def test_required_secret_fails(self) -> None:
        text = self.containerfile.replace("required=false", "required=true", 1)
        self.assertTrue(any("required=false" in error for error in self.errors(containerfile=text)))

    def test_missing_compose_source_or_build_grant_fails(self) -> None:
        for needle in ("    environment: GITHUB_TOKEN\n", "          target: github_token\n"):
            with self.subTest(needle=needle.strip()):
                text = self.compose.replace(needle, "", 1)
                self.assertTrue(any("Compose" in error for error in self.errors(compose=text)))

    def test_missing_raw_build_wiring_fails(self) -> None:
        text = self.workflow.replace("--secret id=github_token,env=GITHUB_TOKEN", "", 1)
        self.assertTrue(any("raw CI build" in error for error in self.errors(workflow=text)))

    def test_missing_anonymous_build_documentation_fails(self) -> None:
        text = self.docs.replace("env -u GITHUB_TOKEN docker build", "docker build", 1)
        self.assertTrue(any("anonymous raw-build" in error for error in self.errors(docs=text)))


if __name__ == "__main__":
    unittest.main()
