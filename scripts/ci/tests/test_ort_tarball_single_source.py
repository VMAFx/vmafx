# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check ONNX source restrictions and the ClangBuildAnalyzer guard."""

from __future__ import annotations

import json
from pathlib import Path

from scripts.ci import check_ort_tarball_pins as guard

ROOT = Path(__file__).resolve().parents[3]


def test_old_source_and_direct_nuget_refused(tmp_path: Path) -> None:
    workflow = tmp_path / "test.yml"
    for source, message in (
        ("https://github.com/microsoft/onnxruntime/releases/download/v1/file", "old ONNX"),
        ("https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/file", "must use helper"),
    ):
        workflow.write_text(f"run: curl {source}\n", encoding="utf-8")
        rc, findings = guard.check_tarball_pins(files=[workflow])
        assert rc == 1
        assert message in "\n".join(findings)


def test_bad_pin_refused(tmp_path: Path) -> None:
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


def test_workflow_helper_version_pin_positive_and_negative(tmp_path: Path) -> None:
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


def test_cba_guard(tmp_path: Path) -> None:
    source = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")
    container = tmp_path / "Containerfile"
    container.write_text(source, encoding="utf-8")
    assert guard.check_tarball_pins(files=[container])[0] == 0
    container.write_text(
        source.replace("ARG CLANGBUILDANALYZER_SHA256=", "ARG REMOVED="), encoding="utf-8"
    )
    rc, findings = guard.check_tarball_pins(files=[container])
    assert rc == 1
    assert "missing CLANGBUILDANALYZER_SHA256" in "\n".join(findings)


def test_cba_digest_and_order_boundaries(tmp_path: Path) -> None:
    source = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")
    container = tmp_path / "Containerfile"
    pin = "868a8d34ecb9b65da4e5874342062a12c081ce4385c7ddd6ce7d557a0c5c292d"
    check = '    && echo "${CLANGBUILDANALYZER_SHA256}  /tmp/cba.tar.gz" | sha256sum -c - \\\n'
    extract = "    && tar -xzf /tmp/cba.tar.gz -C /tmp \\\n"
    assert pin in source and check + extract in source
    for mutated, finding in (
        (source.replace(pin, "a" * 63), "malformed CLANGBUILDANALYZER_SHA256"),
        (source.replace(check, ""), "missing sha256sum --check"),
        (source.replace(check + extract, extract + check), "tar extraction occurs before"),
    ):
        container.write_text(mutated, encoding="utf-8")
        rc, findings = guard.check_tarball_pins(files=[container])
        assert rc == 1
        assert finding in "\n".join(findings)
