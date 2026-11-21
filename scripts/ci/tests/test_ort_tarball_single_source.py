# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check ONNX source restrictions and the ClangBuildAnalyzer guard."""

from __future__ import annotations

import json
import os
import re
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from scripts.ci import check_ort_tarball_pins as guard
from scripts.lib.safe_subprocess import CommandValidationError
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
SHAPE_FINDING = "container ort_nuget_install.py instruction violates canonical RUN shape"


class TestOrtGuard(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def test_old_source_and_direct_nuget_refused(self) -> None:
        tmp_path = Path(self.temp.name)
        workflow = tmp_path / "test.yml"
        for source, message in (
            ("https://github.com/microsoft/onnxruntime/releases/download/v1/file", "old ONNX"),
            (
                "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/file",
                "must use helper",
            ),
        ):
            workflow.write_text(f"run: curl {source}\n", encoding="utf-8")
            rc, findings = guard.check_tarball_pins(files=[workflow])
            assert rc == 1
            assert message in "\n".join(findings)

    def test_bad_pin_refused(self) -> None:
        tmp_path = Path(self.temp.name)
        pins = tmp_path / "scripts/ci/ort-nuget-pins.json"
        pins.parent.mkdir(parents=True)
        for bad_entry, message in (
            ({"sha256": "a" * 63, "size": 10}, "malformed sha256"),
            ({"sha256": "a" * 64, "size": 0}, "invalid size"),
            ({"sha256": "a" * 64, "size": -5}, "invalid size"),
        ):
            pins.write_text(
                json.dumps({"packages": {"cpu": {"versions": {"1.0": bad_entry}}}}),
                encoding="utf-8",
            )
            rc, findings = guard.check_tarball_pins(root=tmp_path)
            assert rc == 1
            assert message in "\n".join(findings)

    def test_workflow_helper_version_pin_positive_and_negative(self) -> None:
        tmp_path = Path(self.temp.name)
        workflow = tmp_path / "workflow.yml"
        workflow.write_text(
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - name: Install CPU ORT\n"
            "        env:\n"
            '          ORT_VERSION: "1.29.0"\n'
            "        run: |\n"
            '          python3 scripts/ci/ort_nuget_install.py --version "${ORT_VERSION}" \\\n'
            "            --prefix /opt/onnxruntime\n"
            "      - name: Install GPU ORT\n"
            "        env:\n"
            '          ORT_VERSION: "1.22.0"\n'
            "        run: |\n"
            '          python3 scripts/ci/ort_nuget_install.py --package gpu --version "${ORT_VERSION}" \\\n'
            "            --prefix /opt/onnxruntime\n",
            encoding="utf-8",
        )
        rc, findings = guard.check_tarball_pins(files=[workflow])
        assert rc == 0, findings

        for pkg_arg, unpinned_ver, pkg_label in (
            ("", "1.99.0", "cpu"),
            ("--package gpu ", "1.29.0", "gpu"),
        ):
            workflow.write_text(
                "jobs:\n"
                "  build:\n"
                "    steps:\n"
                "      - name: Install ORT\n"
                "        env:\n"
                f'          ORT_VERSION: "{unpinned_ver}"\n'
                "        run: |\n"
                f'          python3 scripts/ci/ort_nuget_install.py {pkg_arg}--version "${{ORT_VERSION}}" \\\n'
                "            --prefix /opt/onnxruntime\n",
                encoding="utf-8",
            )
            rc, findings = guard.check_tarball_pins(files=[workflow])
            assert rc == 1
            assert f"unpinned ORT_VERSION '{unpinned_ver}' for package '{pkg_label}'" in "\n".join(
                findings
            )

    def test_multiple_helper_commands_use_their_own_arguments(self) -> None:
        workflow = Path(self.temp.name) / "commands.yml"
        packages: dict[str, dict[str, Any]] = {
            "cpu": {"versions": {"1.0": {}}},
            "gpu": {"versions": {"2.0": {}}},
        }
        first = "    python ort_nuget_install.py --package cpu --version 1.0\n"
        second = "    python ort_nuget_install.py --package gpu --version 2.0\n"
        workflow.write_text("- run: |\n" + first + second)
        self.assertEqual(guard.scan_file(workflow, packages)[0], [])
        workflow.write_text("- run: |\n" + first + second.replace("2.0", "1.0"))
        findings = guard.scan_file(workflow, packages)[0]
        self.assertEqual(len(findings), 1)
        self.assertIn(":3: unpinned ORT_VERSION '1.0' for package 'gpu'", findings[0])

    def test_helper_version_bindings(self) -> None:
        workflow = Path(self.temp.name) / "bindings.yml"
        packages: dict[str, dict[str, Any]] = {"cpu": {"versions": {"1.0": {}}}}
        prefix = '- name: install\n  env:\n    ORT_VERSION: "1.0"\n  run: |\n'
        call = '    python ort_nuget_install.py --version "$ORT_VERSION"\n'
        for assignment, expected in (
            ("", False),
            ("    ORT_VERSION=1.0\n", False),
            ("    ORT_VERSION=9.0\n", True),
            ("    # ORT_VERSION=9.0\n", False),
        ):
            with self.subTest(assignment=assignment):
                workflow.write_text(prefix + assignment + call)
                findings = guard.scan_file(workflow, packages)[0]
                self.assertEqual(bool(findings), expected, findings)
                if expected:
                    self.assertIn(":6: ambiguous ORT_VERSION in step", findings[0])

    def test_nuget_package_identity(self) -> None:
        workflow = Path(self.temp.name) / "downloads.yml"
        address = "https://api.nuget.org/v3-flatcontainer/"
        cases = (
            (f"curl {address}microsoft.ml.onnxruntime/file", "must use helper"),
            (
                f"ORT_PACKAGE: microsoft.ml.onnxruntime\n  run: curl {address}${{ORT_PACKAGE}}/file",
                "must use helper",
            ),
            (f"run: wget {address}$UNKNOWN/file", "NuGet download with unresolved package"),
            (f"run: curl {address}other.package/file", None),
            (f"# curl {address}${{UNKNOWN}}/file", None),
            (f"OTHER: unrelated\n  run: curl {address}$OTHER/file", None),
        )
        for text, expected in cases:
            with self.subTest(text=text):
                workflow.write_text("- name: download\n  " + text + "\n")
                findings = guard.scan_file(workflow, {})[0]
                if expected:
                    self.assertIn(expected, "\n".join(findings))
                else:
                    self.assertEqual(findings, [])

    def test_docker_env_reads_last_effective_assignment_and_continuation(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        call = (
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu "
            '--version "${ORT_VERSION}" --prefix /usr/local\n'
        )
        cases = (
            "ARG ORT_VERSION=1.30.0\nENV OTHER=foo ORT_VERSION=1.99.0\n" + call,
            "ARG ORT_VERSION=1.30.0\nENV A=1 \\\n    ORT_VERSION=1.99.0\n" + call,
        )
        for text in cases:
            with self.subTest(text=text):
                container.write_text(text)
                rc, findings = guard.check_tarball_pins(files=[container])
                self.assertEqual(rc, 1, findings)
                self.assertIn(
                    "unpinned ORT_VERSION '1.99.0' for package 'cpu'", "\n".join(findings)
                )

    def test_helper_token_must_be_one_canonical_logical_instruction(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            "RUN ort_nuget_install.py --package cpu --version 1.30.0 --prefix /usr/local\n",
            "RUN python3 /tmp/ort_nuget_install.py --package cpu --version 1.30.0 --prefix /usr/local\n",
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0"
            " && echo 'quoted && marker'\n",
        )
        for text in cases:
            with self.subTest(text=text):
                container.write_text(text)
                rc, findings = guard.check_tarball_pins(files=[container])
                self.assertEqual(rc, 1)
                self.assertEqual(findings, [f"{container}:1: {SHAPE_FINDING}"])

    def test_container_shell_segments_split_only_unquoted_and(self) -> None:
        quoted = ('echo "a && b"', "echo 'c && d'", r"echo e\&&f")
        commands = (*quoted, "ldconfig")
        self.assertEqual(
            [segment.strip() for segment in guard._shell_segments(" && ".join(commands))],
            list(commands),
        )

    def test_container_helper_version_variable_uses_shell_quoting(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        for version in ("$ORT_VERSION", '"$ORT_VERSION"', "${ORT_VERSION}", '"${ORT_VERSION}"'):
            with self.subTest(version=version):
                container.write_text(
                    "ARG ORT_VERSION=1.30.0\n"
                    "RUN set -eux && python3 scripts/ci/ort_nuget_install.py --package cpu "
                    f"--version {version} --prefix /usr/local\n",
                    encoding="utf-8",
                )
                self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        for version in ("'$ORT_VERSION'", "'${ORT_VERSION}'"):
            with self.subTest(version=version):
                container.write_text(
                    "ARG ORT_VERSION=1.30.0\n"
                    "RUN set -eux && python3 scripts/ci/ort_nuget_install.py --package cpu "
                    f"--version {version} --prefix /usr/local\n",
                    encoding="utf-8",
                )
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:2: {SHAPE_FINDING}"]),
                )

    def test_a_command_outside_the_allowlist_invalidates_a_helper_run(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN echo ok && python3 scripts/ci/ort_nuget_install.py --package cpu "
            '--version "${ORT_VERSION}" --prefix /usr/local\n'
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:2: {SHAPE_FINDING}"]),
        )

    def test_container_sources_and_pins(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        for keyword in ("ARG", "ENV"):
            helper = (
                f"{keyword} ORT_VERSION={{version}}\n"
                "RUN python3 scripts/ci/ort_nuget_install.py --package cpu " + chr(92) + "\n"
                '    --version "${{ORT_VERSION}}" --prefix /usr/local\n'
            )
            container.write_text(helper.format(version="1.30.0"))
            self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
            container.write_text(helper.format(version="1.99.0"))
            self.assertEqual(
                guard.check_tarball_pins(files=[container]),
                (1, [f"{container}:2: unpinned ORT_VERSION '1.99.0' for package 'cpu'"]),
            )
        for command in (
            "RUN curl https://github.com/microsoft/onnxruntime/releases/download/v1/file\n",
            "RUN curl https://github.com/microsoft/onnxruntime/releases/\\\n    download/v1/file\n",
            "RUN curl https://api.nuget.org/v3-flatcontainer/\\\n    microsoft.ml.onnxruntime/file\n",
        ):
            container.write_text(command)
            rc, findings = guard.check_tarball_pins(files=[container])
            self.assertEqual(rc, 1, findings)
            self.assertIn(f"{container}:1:", "\n".join(findings))

    def test_container_continuation_comment_helper_accepted(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN set -eux \\\n"
            "    # comment\n"
            "    && python3 scripts/ci/ort_nuget_install.py --package cpu \\\n"
            "        # comment\n"
            '        --version "${ORT_VERSION}" --prefix /usr/local\n'
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_bindings_share_comment_and_continuation_rules(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "FROM base\n"
            "ARG \\\n"
            "    # continued ARG\n"
            "    ORT_VERSION=1.30.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py \\\n"
            "    # continued RUN\n"
            "    --version $ORT_VERSION --prefix /usr/local\n"
            "ENV UNUSED= \\\n"
            "    # continued ENV\n"
            "    ORT_VERSION=1.30.0\n"
            "ARG ORT_VERSION=1.99.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --version $ORT_VERSION --prefix /usr/local\n",
            encoding="utf-8",
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_stage_bindings_use_last_arg_and_env(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        call = (
            "RUN python3 scripts/ci/ort_nuget_install.py --version $ORT_VERSION --prefix /opt/ort\n"
        )
        for declarations in (
            "ARG ORT_VERSION=1.99.0\nARG ORT_VERSION=1.30.0\n",
            "ENV ORT_VERSION=1.99.0\nENV UNUSED= ORT_VERSION=1.30.0\n",
            "ENV ORT_VERSION=1.30.0\nARG ORT_VERSION=1.99.0\n",
        ):
            with self.subTest(declarations=declarations):
                container.write_text("FROM base\n" + declarations + call, encoding="utf-8")
                self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_stage_boundaries_ignore_continuation_and_heredoc_text(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        call = (
            "RUN python3 scripts/ci/ort_nuget_install.py --version $ORT_VERSION --prefix /opt/ort\n"
        )
        source = (
            "FROM base AS build\n"
            "ARG ORT_VERSION=1.30.0\n"
            "RUN printf '%s' \\\n"
            "FROM continued\n"
            "RUN cat <<'TEXT'\n"
            "FROM payload\n"
            "ENV ORT_VERSION=1.99.0\n"
            "TEXT\n"
        )
        container.write_text(source + call, encoding="utf-8")
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(source + "FROM other AS fresh\n" + call, encoding="utf-8")
        rc, findings = guard.check_tarball_pins(files=[container])
        self.assertEqual(rc, 1, findings)
        self.assertIn("ORT_VERSION", "\n".join(findings))

    def test_container_quoted_or_commented_heredoc_marker_is_not_syntax(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "FROM base\n"
            "ARG ORT_VERSION=1.30.0\n"
            "RUN printf '%s' '<<TEXT'\n"
            "RUN printf '%s' \\\n"
            "    # <<IGNORED\n"
            "    okay\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --version $ORT_VERSION --prefix /opt/ort\n",
            encoding="utf-8",
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_unresolved_version_assignment_fails_closed_if_used(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        source = "FROM base\nARG ORT_VERSION=1.30.0\nENV ORT_VERSION=$UNKNOWN_VERSION\n"
        call = "RUN python3 scripts/ci/ort_nuget_install.py --version {} --prefix /opt/ort\n"
        container.write_text(source + call.format("1.30.0"), encoding="utf-8")
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(source + call.format("$ORT_VERSION"), encoding="utf-8")
        rc, findings = guard.check_tarball_pins(files=[container])
        self.assertEqual(rc, 1, findings)
        self.assertIn("ORT_VERSION", "\n".join(findings))

    def test_container_continuation_comment_split_release_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN set -eux \\\n"
            "    # comment\n"
            "    && curl -fsSL https://github.com/microsoft/onnxruntime/releases/\\\n"
            "        download/v1.30.0/ort.tgz\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: old ONNX Runtime release source forbidden"]),
        )

    def test_container_add_continuation_split_release_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ADD https://github.com/microsoft/onnxruntime/releases/\\\n"
            "    # comment\n"
            "    download/v1.30.0/ort.tgz /tmp/ort.tgz\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: old ONNX Runtime release source forbidden"]),
        )

    def test_container_inline_version_override_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN ORT_VERSION=1.99.0 python3 scripts/ci/ort_nuget_install.py"
            ' --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n'
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:2: {SHAPE_FINDING}"]),
        )

    def test_container_continued_inline_version_override_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN ORT_VERSION=1.99.0 \\\n"
                '    python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
                2,
            ),
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN true \\\n"
                "    && ORT_VERSION=1.99.0 \\\n"
                '    python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
                2,
            ),
        )
        for text, line_no in cases:
            with self.subTest(line_no=line_no):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:{line_no}: {SHAPE_FINDING}"]),
                )

    def test_container_quoted_hash_before_split_release_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        for step_arg in ('"step #1"', "'step #1'"):
            with self.subTest(step_arg=step_arg):
                container.write_text(
                    f"RUN echo {step_arg} \\\n"
                    "    && curl -fsSL https://github.com/microsoft/onnxruntime/releases/\\\n"
                    "        download/v1.30.0/ort.tgz\n"
                )
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:1: old ONNX Runtime release source forbidden"]),
                )

    def test_container_continuation_blank_line_split_release_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN set -eux \\\n"
            "\n"
            "    && curl -fsSL https://github.com/microsoft/onnxruntime/releases/\\\n"
            "        download/v1.30.0/ort.tgz\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: old ONNX Runtime release source forbidden"]),
        )

    def test_container_accepted_comments_and_arg_version(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            "ARG ORT_VERSION=1.30.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py \\\n"
            "    --package cpu \\\n"
            "    # comment between continued lines\n"
            '    --version "${ORT_VERSION}" --prefix /usr/local\n',
            "# https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/ort.tgz\n"
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
        )
        for text in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_inline_override_is_a_shape_finding(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            "ARG ORT_VERSION=1.30.0\n"
            "RUN ORT_VERSION=1.30.0 \\\n"
            '    python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
            "ARG ORT_VERSION=1.99.0\n"
            "RUN ORT_VERSION=1.30.0 \\\n"
            '    python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
            "ARG ORT_VERSION=1.30.0\n"
            "RUN ORT_VERSION=1.30.0"
            ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
        )
        for text in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:2: {SHAPE_FINDING}"]),
                )

    def test_container_first_line_and_eof_without_newline_boundaries(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0 --prefix /usr/local\n"
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.99.0 --prefix /usr/local\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: unpinned ORT_VERSION '1.99.0' for package 'cpu'"]),
        )
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu \\\n"
            '    --version "${ORT_VERSION}" --prefix /usr/local'
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN ORT_VERSION=1.99.0 \\\n"
            '    python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local'
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:2: {SHAPE_FINDING}"]),
        )

    def test_container_continuation_line_limit_boundary(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        call = "    && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0 --prefix /usr/local\n"
        at_limit = (
            "RUN ldconfig \\\n" + "    && ldconfig \\\n" * (guard.MAX_COMMAND_LINES - 2) + call
        )
        container.write_text(at_limit)
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        for line_count in (guard.MAX_COMMAND_LINES + 1, guard.MAX_COMMAND_LINES + 20):
            with self.subTest(line_count=line_count):
                over_limit = "RUN ldconfig \\\n" + "    && ldconfig \\\n" * (line_count - 2) + call
                container.write_text(over_limit)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:1: container command continuation exceeds limit"]),
                )

    def test_container_second_helper_call_unpinned_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        two_lines = (
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a \\\n'
            " && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.99.0 --prefix /b\n"
        )
        one_line = (
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a'
            " && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.99.0 --prefix /b\n"
        )
        for text, line_no in ((two_lines, 3), (one_line, 2)):
            with self.subTest(line_no=line_no):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (
                        1,
                        [f"{container}:{line_no}: unpinned ORT_VERSION '1.99.0' for package 'cpu'"],
                    ),
                )

    def test_container_first_helper_call_unpinned_second_pinned_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.99.0 --prefix /a \\\n"
            ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /b\n'
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:2: unpinned ORT_VERSION '1.99.0' for package 'cpu'"]),
        )

    def test_container_standalone_and_export_binding_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN ORT_VERSION=1.99.0"
                ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a\n',
                2,
                SHAPE_FINDING,
            ),
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN export ORT_VERSION=1.99.0"
                ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a\n',
                2,
                "unpinned ORT_VERSION '1.99.0' for package 'cpu'",
            ),
            (
                "RUN ORT_VERSION=1.99.0 \\\n"
                " && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0 --prefix /a \\\n"
                ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /b\n',
                1,
                SHAPE_FINDING,
            ),
            (
                "RUN export ORT_VERSION=1.99.0 \\\n"
                " && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0 --prefix /a \\\n"
                ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /b\n',
                3,
                "unpinned ORT_VERSION '1.99.0' for package 'cpu'",
            ),
        )
        for text, line_no, message in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:{line_no}: {message}"]),
                )

    def test_container_measured_bypass_shapes_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN sh -c 'ORT_VERSION=1.99.0 python3 scripts/ci/ort_nuget_install.py"
                ' --package cpu --version "${ORT_VERSION}" --prefix /usr/local\'\n',
                2,
            ),
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN sh -c 'ORT_VERSION=1.99.0 && python3 scripts/ci/ort_nuget_install.py"
                ' --package cpu --version "${ORT_VERSION}" --prefix /usr/local\'\n',
                2,
            ),
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN (ORT_VERSION=1.99.0 && python3 scripts/ci/ort_nuget_install.py"
                ' --package cpu --version "${ORT_VERSION}" --prefix /usr/local)\n',
                2,
            ),
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN --mount=type=cache,target=/tmp/cache ORT_VERSION=1.99.0 \\\n"
                "&& python3 scripts/ci/ort_nuget_install.py --package cpu"
                ' --version "${ORT_VERSION}" --prefix /usr/local\n',
                2,
            ),
            (
                "ARG ORT_VERSION=1.30.0\n"
                "RUN python3 scripts/ci/ort_nuget_install.py --package cpu"
                ' --version "${ORT_VERSION}" --prefix /a \\\n'
                "& python3 scripts/ci/ort_nuget_install.py --package cpu"
                " --version 1.99.0 --prefix /b\n",
                2,
            ),
            (
                "RUN python3 scripts/ci/ort_nuget_install.py --package cpu"
                " --version 1.30.0 --version 1.99.0 --prefix /usr/local\n",
                1,
            ),
        )
        for text, line_no in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:{line_no}: {SHAPE_FINDING}"]),
                )

    def test_container_non_canonical_shapes_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cmd = "python3 scripts/ci/ort_nuget_install.py --package cpu"
        cases = (
            (
                'RUN ["python3", "scripts/ci/ort_nuget_install.py", "--package", "cpu",'
                ' "--version", "1.30.0", "--prefix", "/usr/local"]\n',
                1,
            ),
            (f"RUN <<EOF\n{cmd} --version 1.30.0 --prefix /usr/local\nEOF\n", 1),
            (f"RUN <<EOF \\\n{cmd} --version 1.30.0 --prefix /usr/local\n", 1),
            (
                f'ARG ORT_VERSION=1.30.0\nRUN false || {cmd} --version "${{ORT_VERSION}}" --prefix /usr/local\n',
                2,
            ),
            (
                f'ARG ORT_VERSION=1.30.0\nRUN export ORT_VERSION=1.30.0; {cmd} --version "${{ORT_VERSION}}" --prefix /usr/local\n',
                2,
            ),
            (f"RUN {cmd} --version $(echo 1.30.0) --prefix /usr/local\n", 1),
            (
                f'ARG ORT_VERSION=1.30.0\nRUN env ORT_VERSION=1.99.0 {cmd} --version "${{ORT_VERSION}}" --prefix /usr/local\n',
                2,
            ),
            (
                f'RUN : ${{ORT_VERSION:=1.99.0}} && {cmd} --version "${{ORT_VERSION}}" --prefix /usr/local\n',
                1,
            ),
            (f"RUN unset ORT_VERSION && {cmd} --version 1.30.0 --prefix /usr/local\n", 1),
            (f"RUN eval true && {cmd} --version 1.30.0 --prefix /usr/local\n", 1),
            (f"RUN {cmd} --version 1.30.0 --prefix /usr/local --unknown-flag val\n", 1),
            (f"RUN {cmd} --package gpu --version 1.30.0 --prefix /usr/local\n", 1),
        )
        for text, line_no in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:{line_no}: {SHAPE_FINDING}"]),
                )

    def test_container_canonical_shapes_and_installer_flags_accepted(self) -> None:
        installer_src = (ROOT / "scripts/ci/ort_nuget_install.py").read_text(encoding="utf-8")
        installer_flags = set(re.findall(r'add_argument\("(--[a-z-]+)"', installer_src))
        self.assertEqual(installer_flags, getattr(guard, "HELPER_FLAGS", set()))
        sample_values = {
            "--package": "cpu",
            "--version": "1.30.0",
            "--prefix": "/usr/local",
            "--pkgconfig-dir": "/usr/local/lib/pkgconfig",
            "--cache-dir": "/var/cache/ort",
            "--print-pin": "1.30.0",
        }
        self.assertEqual(set(sample_values), installer_flags)
        all_flags = " ".join(
            f"{flag} {sample_values[flag]}"
            for flag in sorted(installer_flags)
            if flag != "--print-pin"
        )
        container = Path(self.temp.name) / "Containerfile"
        positives = (
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
            "ARG ORT_VERSION=1.30.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu \\\n"
            '    --version "${ORT_VERSION}" --prefix /usr/local\n',
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0 --prefix /usr/local\n",
            "RUN python3 scripts/ci/ort_nuget_install.py --version 1.30.0 --package cpu --prefix /usr/local\n",
            "RUN export ORT_VERSION=1.30.0"
            ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /usr/local\n',
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a \\\n'
            " && python3 scripts/ci/ort_nuget_install.py --package gpu --version 1.22.0 --prefix /b\n",
            "ARG ORT_VERSION=1.30.0\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu \\\n"
            "    # comment between continued lines\n"
            '    --version "${ORT_VERSION}" --prefix /usr/local\n',
            f"RUN python3 scripts/ci/ort_nuget_install.py {all_flags}\n",
        )
        for text in positives:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_print_pin_is_a_shape_finding(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu "
            "--version 1.30.0 --prefix /usr/local --print-pin 1.30.0\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: {SHAPE_FINDING}"]),
        )

    def test_container_comment_with_helper_token_before_call(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN set -eux \\\n"
            "    # ort_nuget_install.py comment between continued lines\n"
            '    && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a\n'
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a \\\n'
            "    # ort_nuget_install.py comment between continued lines\n"
            "    && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.99.0 --prefix /b\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:4: unpinned ORT_VERSION '1.99.0' for package 'cpu'"]),
        )

    def test_container_inline_assignment_first_call_only_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            "RUN ORT_VERSION=1.99.0 python3 scripts/ci/ort_nuget_install.py"
            ' --package cpu --version "${ORT_VERSION}" --prefix /a \\\n'
            ' && python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /b\n'
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:2: {SHAPE_FINDING}"]),
        )

    def test_container_multiple_pinned_calls_and_continued_version_accepted(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a \\\n'
            " && python3 scripts/ci/ort_nuget_install.py --package gpu --version 1.22.0 --prefix /b\n",
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0 \\\n"
            "--prefix /usr/local\n",
        )
        for text in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_three_calls_middle_unpinned_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "ARG ORT_VERSION=1.30.0\n"
            'RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version "${ORT_VERSION}" --prefix /a \\\n'
            " && python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.99.0 --prefix /b \\\n"
            " && python3 scripts/ci/ort_nuget_install.py --package gpu --version 1.22.0 --prefix /c\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:3: unpinned ORT_VERSION '1.99.0' for package 'cpu'"]),
        )

    def test_container_helper_token_in_flag_value_is_shape(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu "
            "--version 1.30.0 --prefix /usr/local --cache-dir /tmp/scripts/ci/ort_nuget_install.py\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: {SHAPE_FINDING}"]),
        )

    def test_container_dot_and_source_commands_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        for prologue in (". /vars", "source /vars"):
            container.write_text(
                "ARG ORT_VERSION=1.30.0\n"
                f"RUN {prologue} && python3 scripts/ci/ort_nuget_install.py "
                '--package cpu --version "${ORT_VERSION}" --prefix /usr/local\n'
            )
            self.assertEqual(
                guard.check_tarball_pins(files=[container]),
                (1, [f"{container}:2: {SHAPE_FINDING}"]),
            )

    def test_container_module_form_without_pin_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN python3 -m scripts.ci.ort_nuget_install --package cpu "
            "--version 1.99.0 --prefix /usr/local\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: unpinned ORT_VERSION '1.99.0' for package 'cpu'"]),
        )

    def test_container_package_suffix_and_missing_prefix_refused(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        cases = (
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu.extra "
            "--version 1.30.0 --prefix /usr/local\n",
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu --version 1.30.0\n",
        )
        for text in cases:
            with self.subTest(text=text):
                container.write_text(text)
                self.assertEqual(
                    guard.check_tarball_pins(files=[container]),
                    (1, [f"{container}:1: {SHAPE_FINDING}"]),
                )

    def test_container_arg_survives_workdir_and_unrelated_nuget_variable(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "FROM base\n"
            "ARG ORT_VERSION=1.30.0\n"
            "WORKDIR /build\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu "
            '--version "${ORT_VERSION}" --prefix /usr/local\n'
            "ARG NUGET_PACKAGE=some.other.package\n"
            "RUN curl https://api.nuget.org/v3-flatcontainer/${NUGET_PACKAGE}/file\n"
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_nuget_checks_every_url_variable_in_one_command(self) -> None:
        address = "https://api.nuget.org/v3-flatcontainer/"
        download = f"curl {address}$FIRST/file {address}${{SECOND}}/file\n"
        root = Path(self.temp.name)
        for filename, prefix, assignment in (
            ("download.yml", "- run: ", "  {}: {}\n"),
            ("Containerfile", "RUN ", "ARG {}={}\n"),
        ):
            path = root / filename
            for second, expected in (
                ("other.package", None),
                ("microsoft.ml.onnxruntime.gpu", "ONNX Runtime download must use helper"),
                (None, "NuGet download with unresolved package"),
            ):
                with self.subTest(filename=filename, second=second):
                    declarations = assignment.format("FIRST", "other.package")
                    if second is not None:
                        declarations += assignment.format("SECOND", second)
                    if filename.endswith(".yml"):
                        # Both definitions belong to one step, before the run key.
                        text = (
                            "- name: download\n  env:\n"
                            + "".join(
                                "  " + line for line in declarations.splitlines(keepends=True)
                            )
                            + "  run: "
                            + download
                        )
                    else:
                        text = "FROM base\n" + declarations + prefix + download
                    path.write_text(text, encoding="utf-8")
                    rc, findings = guard.check_tarball_pins(files=[path])
                    self.assertEqual(rc, int(expected is not None), findings)
                    self.assertEqual(len(findings), int(expected is not None))
                    if expected is not None:
                        self.assertIn(expected, findings[0])

    def test_container_nuget_uses_only_referenced_effective_stage_value(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        source = (
            "FROM base AS first\n"
            "ARG PACKAGE=microsoft.ml.onnxruntime\n"
            "FROM other AS second\n"
            "ENV UNUSED_ORT=microsoft.ml.onnxruntime\n"
            "ENV PACKAGE={}\n"
            "ARG PACKAGE=microsoft.ml.onnxruntime\n"
            "RUN curl https://api.nuget.org/v3-flatcontainer/${{PACKAGE}}/file\n"
        )
        container.write_text(source.format("other.package"), encoding="utf-8")
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(source.format("microsoft.ml.onnxruntime"), encoding="utf-8")
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:7: ONNX Runtime download must use helper"]),
        )
        source = source.replace("ENV PACKAGE={}\nARG PACKAGE=microsoft.ml.onnxruntime\n", "")
        container.write_text(source.format(), encoding="utf-8")
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:5: NuGet download with unresolved package"]),
        )

    def test_container_long_unrelated_run_is_ignored(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        container.write_text(
            "RUN echo start \\\n"
            + "    && echo step \\\n" * (guard.MAX_COMMAND_LINES + 20)
            + "    && echo end\n"
        )
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))

    def test_container_yaml_suffix_uses_container_parser(self) -> None:
        container = Path(self.temp.name) / "Containerfile.yaml"
        container.write_text(
            "RUN curl https://api.nuget.org/v3-flatcontainer/\\\n"
            "    microsoft.ml.onnxruntime/file\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(files=[container]),
            (1, [f"{container}:1: ONNX Runtime download must use helper"]),
        )

    def test_cba_guard(self) -> None:
        tmp_path = Path(self.temp.name)
        source = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")
        container = tmp_path / "Containerfile"
        container.write_text(source, encoding="utf-8")
        assert guard.check_tarball_pins(files=[container])[0] == 0

    def test_cba_digest_and_order_boundaries(self) -> None:
        tmp_path = Path(self.temp.name)
        container = tmp_path / "Containerfile"
        pin = "a" * 64
        check = "    && sha256sum --check /tmp/check \\\n"
        extract = "    && tar -xzf /tmp/cba.tar.gz \\\n"
        source = (
            f"ARG CLANGBUILDANALYZER_SHA256={pin}\n"
            "RUN curl https://github.com/aras-p/ClangBuildAnalyzer/archive/v1.tar.gz \\\n"
            + check
            + extract
            + "    && true\n"
        )
        for mutated, finding in (
            (
                source.replace("CLANGBUILDANALYZER_SHA256=", "REMOVED="),
                "missing CLANGBUILDANALYZER_SHA256",
            ),
            (source.replace(pin, "a" * 63), "malformed CLANGBUILDANALYZER_SHA256"),
            (source.replace(check, ""), "missing sha256sum --check"),
            (source.replace(check + extract, extract + check), "tar extraction occurs before"),
        ):
            container.write_text(mutated, encoding="utf-8")
            rc, findings = guard.check_tarball_pins(files=[container])
            assert rc == 1
            assert finding in "\n".join(findings)

    def test_cba_order_ignores_unrelated_checks_and_extraction_before_download(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        check = "    && sha256sum --check /tmp/cba.sha256 \\\n"
        extract = "    && tar -xf /tmp/cba.tar.gz \\\n"
        preceding = (
            f"ARG CLANGBUILDANALYZER_SHA256={'a' * 64}\n"
            "RUN sha256sum --check /tmp/other.sha256 \\\n"
            "    && tar -xf /tmp/other.tar.gz \\\n"
            "    && curl -o /tmp/cba.tar.gz https://github.com/aras-p/ClangBuildAnalyzer/archive/v1.tar.gz \\\n"
        )
        ordered = preceding + check + extract + "    && true\n"
        for text, expected in (
            (ordered, None),
            (ordered.replace(" \\\n    ", " "), None),
            (ordered.replace(check, ""), "missing sha256sum --check"),
            (ordered.replace(check + extract, extract + check), "tar extraction occurs before"),
        ):
            with self.subTest(text=text):
                container.write_text(text, encoding="utf-8")
                rc, findings = guard.check_tarball_pins(files=[container])
                self.assertEqual(rc, int(expected is not None), findings)
                if expected is None:
                    self.assertEqual(findings, [])
                else:
                    self.assertIn(expected, "\n".join(findings))

    def test_cba_workflow_order_starts_at_download_even_on_same_line(self) -> None:
        workflow = Path(self.temp.name) / "workflow.yml"
        pin = "a" * 64
        check = "sha256sum --check /tmp/cba.sha256"
        extract = "tar -xf /tmp/cba.tar.gz"
        preceding = (
            "- name: Download CBA\n"
            "  env:\n"
            f"    CLANGBUILDANALYZER_SHA256: {pin}\n"
            "  run: |\n"
            "    sha256sum --check /tmp/other.sha256 && tar -xf /tmp/other.tar.gz\n"
            "    curl -o /tmp/cba.tar.gz https://github.com/aras-p/ClangBuildAnalyzer/archive/v1.tar.gz"
        )
        for steps, expected in (
            (f"\n    {check}\n    {extract}", None),
            (f" && {check} && {extract}", None),
            (f"\n    {extract}", "missing sha256sum --check"),
            (f" && {extract} && {check}", "tar extraction occurs before"),
        ):
            with self.subTest(steps=steps):
                workflow.write_text(preceding + steps + "\n", encoding="utf-8")
                rc, findings = guard.check_tarball_pins(files=[workflow])
                self.assertEqual(rc, int(expected is not None), findings)
                if expected is None:
                    self.assertEqual(findings, [])
                else:
                    self.assertIn(expected, "\n".join(findings))

    def test_package_argument_forms_and_literal_version(self) -> None:
        workflow = Path(self.temp.name) / "test.yml"
        for arg in ('--package "gpu"', "--package 'gpu'", "--package gpu", "--package=gpu"):
            workflow.write_text(
                f'- name: install\n  env:\n    ORT_VERSION: "1.29.0"\n'
                f'  run: python3 scripts/ci/ort_nuget_install.py {arg} --version "${{ORT_VERSION}}"\n'
            )
            rc, findings = guard.check_tarball_pins(files=[workflow])
            self.assertEqual(rc, 1)
            self.assertIn("package 'gpu'", "\n".join(findings))
        workflow.write_text(
            '- name: install\n  env:\n    ORT_VERSION: "1.29.0"\n'
            "  run: python3 scripts/ci/ort_nuget_install.py --version 1.99.0\n"
        )
        self.assertIn("1.99.0", "\n".join(guard.check_tarball_pins(files=[workflow])[1]))

    def test_pin_schema_and_targeted_scan(self) -> None:
        root = Path(self.temp.name)
        pins = root / "scripts/ci/ort-nuget-pins.json"
        pins.parent.mkdir(parents=True)
        workflow = root / "test.yaml"
        workflow.write_text("- run: python3 scripts/ci/ort_nuget_install.py --version 1.0\n")
        pkg: dict[str, Any] = {
            "source": "https://example.test",
            "header_dir": "headers",
            "libraries": ["lib"],
            "versions": {"1.0": {"sha256": "a" * 64, "size": 1}},
        }
        for field in ("source", "header_dir", "libraries", "versions"):
            broken = dict(pkg)
            broken.pop(field)
            pins.write_text(json.dumps({"packages": {"cpu": broken}}))
            rc, findings = guard.check_tarball_pins(root=root, files=[workflow])
            self.assertEqual(rc, 1)
            self.assertIn(field, "\n".join(findings))
        pins.write_text(json.dumps({"packages": None, "versions": pkg["versions"]}))
        rc, findings = guard.check_tarball_pins(root=root, files=[workflow])
        self.assertEqual(rc, 1)
        self.assertIn("missing packages table", "\n".join(findings))
        invalid_values: tuple[Any, ...] = (None, [], "bad")
        for invalid in invalid_values:
            broken = dict(pkg, versions=invalid)
            pins.write_text(json.dumps({"packages": {"cpu": broken}}))
            rc, findings = guard.check_tarball_pins(root=root, files=[workflow])
            self.assertEqual(rc, 1)
            self.assertIn("missing versions", "\n".join(findings))
        pkg["versions"]["1.0"]["size"] = 0
        pins.write_text(json.dumps({"packages": {"cpu": pkg}}))
        self.assertIn(
            "invalid size", "\n".join(guard.check_tarball_pins(root=root, files=[workflow])[1])
        )

    def test_comments_and_unrelated_packages(self) -> None:
        workflow = Path(self.temp.name) / "test.yml"
        workflow.write_text(
            "# curl https://example.test/microsoft.ml.onnxruntime.nupkg\n"
            "run: curl https://example.test/unrelated.nupkg # microsoft.ml.onnxruntime\n"
        )
        self.assertEqual(guard.check_tarball_pins(files=[workflow])[0], 0)

    def test_ort_archive_order_and_discovery(self) -> None:
        root = Path(self.temp.name)
        pins = root / "scripts/ci/ort-nuget-pins.json"
        pins.parent.mkdir(parents=True)
        pins.write_text((ROOT / "scripts/ci/ort-nuget-pins.json").read_text())
        container = root / "Containerfile"
        container.write_text(
            "RUN curl https://github.com/microsoft/onnxruntime/releases/download/v1/file\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(root=root),
            (1, [f"{container}:1: old ONNX Runtime release source forbidden"]),
        )
        container.unlink()
        workflows = root / ".github/workflows"
        workflows.mkdir(parents=True)
        workflow = workflows / "test.yaml"
        workflow.write_text(
            "run: curl https://github.com/microsoft/onnxruntime/releases/download/v1/file\n"
        )
        self.assertEqual(
            guard.check_tarball_pins(root=root),
            (1, [f"{workflow}:1: old ONNX Runtime release source forbidden"]),
        )


class TestOrtGuardDiscovery(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        pins = self.root / "scripts/ci/ort-nuget-pins.json"
        pins.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "scripts/ci/ort-nuget-pins.json", pins)

    def git(self, *args: str) -> None:
        env = {
            key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")
        }
        env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        run_command(
            ("git", *args),
            allowed_executables=("git",),
            cwd=self.root,
            env=env,
            check=True,
            capture_output=True,
            timeout_seconds=10,
        )

    def container(self, relative: str, *, clean: bool = False) -> Path:
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        text = (
            "FROM base\n"
            if clean
            else "RUN curl https://api.nuget.org/v3-flatcontainer/\\\n"
            "    microsoft.ml.onnxruntime/file\n"
        )
        target.write_text(text, encoding="utf-8")
        return target

    def test_tracked_discovery_and_container_filename_classification(self) -> None:
        self.git("init", "--quiet", "--template=")
        self.container("Dockerfile", clean=True)
        self.container("Containerfile.backup")
        self.git("add", "--", "Dockerfile")
        self.assertEqual(guard.check_tarball_pins(root=self.root), (0, []))
        yaml = self.container("dev/Containerfile.yaml")
        suffix = self.container("docker/dev/ubuntu-test.Dockerfile")
        self.container("dev/notes.yaml")
        self.git(
            "add",
            "--",
            "dev/Containerfile.yaml",
            "docker/dev/ubuntu-test.Dockerfile",
            "dev/notes.yaml",
        )
        rc, findings = guard.check_tarball_pins(root=self.root)
        self.assertEqual(rc, 1, findings)
        self.assertCountEqual(
            findings,
            [f"{path}:1: ONNX Runtime download must use helper" for path in (yaml, suffix)],
        )

    def test_non_git_discovery_falls_back_to_filename_matching(self) -> None:
        yaml = self.container("dev/Containerfile.yaml")
        suffix = self.container("docker/dev/ubuntu-test.Dockerfile")
        self.container("dev/notes.yaml")
        rc, findings = guard.check_tarball_pins(root=self.root)
        self.assertEqual(rc, 1, findings)
        self.assertCountEqual(
            findings,
            [f"{path}:1: ONNX Runtime download must use helper" for path in (yaml, suffix)],
        )

    def test_git_ls_files_error_does_not_fall_back(self) -> None:
        self.git("init", "--quiet", "--template=")
        self.container("Containerfile")
        (self.root / ".git/index").write_bytes(b"invalid index")
        rc, findings = guard.check_tarball_pins(root=self.root)
        self.assertEqual(rc, 2, findings)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("git ls-files", findings[0])

    def test_git_launch_errors_are_controlled(self) -> None:
        self.git("init", "--quiet", "--template=")
        self.container("Containerfile")
        for error in (OSError("Git unavailable"), CommandValidationError("Git refused")):
            with (
                self.subTest(error=error),
                mock.patch.object(guard, "run_command", side_effect=error),
            ):
                rc, findings = guard.check_tarball_pins(root=self.root)
                self.assertEqual(rc, 2, findings)
                self.assertEqual(len(findings), 1, findings)
                self.assertIn(str(error), findings[0])
