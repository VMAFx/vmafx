#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Verify the tester fixture commit mirrors build-config.env's VMAF_RESOURCE_COMMIT.

`docker/Dockerfile.tester` keeps an `ARG` default because an image build cannot
source `build-config.env` at parse time. The tester-bundle workflows load the
value at run time, and the seven fixture records in the licensing manifest name
it. ``check_vmaf_resource_commit()`` fails on a missing, duplicated or differing
value in any of those places.
"""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

CHECK = MODULE.check_vmaf_resource_commit
WANT = "c0ab6adbd7e41bb354f14686ed08500530622bc3"
RECORD = "tools/rc1-tester/image/licensing.json"


class VmafResourceCommitSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        self.write("docker/Dockerfile.tester", f"ARG VMAF_RESOURCE_COMMIT={WANT}\n")
        self.write_records([WANT] * MODULE.RESOURCE_RECORD_COUNT)

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def write_records(self, commits: list[str]) -> None:
        artifacts = {
            f"artifact-{index}": {
                "components": [
                    {
                        "id": "fixture",
                        "source": (
                            "https://github.com/Netflix/vmaf_resource/tree/"
                            f"{commit}/python/test/resource"
                        ),
                    }
                ]
            }
            for index, commit in enumerate(commits)
        }
        self.write(RECORD, json.dumps({"artifacts": artifacts}))

    def test_matching_mirrors_pass(self) -> None:
        self.assertEqual(CHECK(self.repo, WANT), [])

    def test_repository_mirrors_match_config(self) -> None:
        want = MODULE.load_config(ROOT).get("VMAF_RESOURCE_COMMIT")
        self.assertEqual(CHECK(ROOT, want), [])

    def test_drifted_dockerfile_mirror_fails(self) -> None:
        self.write("docker/Dockerfile.tester", "ARG VMAF_RESOURCE_COMMIT=deadbeef\n")
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("docker/Dockerfile.tester", problems[0])
        self.assertIn("deadbeef", problems[0])

    def test_missing_dockerfile_mirror_fails(self) -> None:
        (self.repo / "docker/Dockerfile.tester").unlink()
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("docker/Dockerfile.tester", problems[0])

    def test_duplicate_dockerfile_mirror_fails(self) -> None:
        self.write(
            "docker/Dockerfile.tester",
            f"ARG VMAF_RESOURCE_COMMIT={WANT}\nARG VMAF_RESOURCE_COMMIT={WANT}\n",
        )
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("exactly once", problems[0])

    def test_drifted_record_fails(self) -> None:
        commits = [WANT] * MODULE.RESOURCE_RECORD_COUNT
        commits[3] = "f" * 40
        self.write_records(commits)
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn(RECORD, problems[0])
        self.assertIn("f" * 40, problems[0])

    def test_missing_record_fails(self) -> None:
        commits = [WANT] * (MODULE.RESOURCE_RECORD_COUNT - 1)
        self.write_records(commits)
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn(f"expected {MODULE.RESOURCE_RECORD_COUNT}", problems[0])

    def test_missing_config_value_fails(self) -> None:
        self.assertEqual(
            CHECK(self.repo, None), ["build-config.env defines no VMAF_RESOURCE_COMMIT"]
        )


if __name__ == "__main__":
    unittest.main()
