# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check ONNX source restrictions and the ClangBuildAnalyzer guard."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from typing import Any

from scripts.ci import check_ort_tarball_pins as guard

ROOT = Path(__file__).resolve().parents[3]


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

    def test_container_sources_and_pins(self) -> None:
        container = Path(self.temp.name) / "Containerfile"
        helper = (
            "ARG ORT_VERSION={version}\n"
            "RUN python3 scripts/ci/ort_nuget_install.py --package cpu " + chr(92) + "\n"
            '    --version "${{ORT_VERSION}}" --prefix /usr/local\n'
        )
        container.write_text(helper.format(version="1.30.0"))
        self.assertEqual(guard.check_tarball_pins(files=[container]), (0, []))
        container.write_text(helper.format(version="1.99.0"))
        self.assertIn(
            "unpinned ORT_VERSION", "\n".join(guard.check_tarball_pins(files=[container])[1])
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
        container = root / "Containerfile"
        container.write_text(
            "RUN curl https://github.com/microsoft/onnxruntime/releases/download/v1/file\n"
        )
        self.assertIn("old ONNX", "\n".join(guard.check_tarball_pins(files=[container])[1]))
        pins = root / "scripts/ci/ort-nuget-pins.json"
        pins.parent.mkdir(parents=True)
        pins.write_text((ROOT / "scripts/ci/ort-nuget-pins.json").read_text())
        workflows = root / ".github/workflows"
        workflows.mkdir(parents=True)
        (workflows / "test.yaml").write_text(
            "run: curl https://github.com/microsoft/onnxruntime/releases/download/v1/file\n"
        )
        self.assertIn("old ONNX", "\n".join(guard.check_tarball_pins(root=root)[1]))
