#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for documentation link and anchor checker."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.docs.check_doc_links import (
    EXIT_FINDINGS,
    EXIT_SUCCESS,
    EXIT_UNREADABLE,
    main,
)


def test_positive(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Verify valid relative files, anchors, and source-tree references."""
    docs_api = tmp_path / "docs" / "api"
    docs_api.mkdir(parents=True)
    core_include = tmp_path / "core" / "include"
    core_include.mkdir(parents=True)

    header = core_include / "libvmaf.h"
    header.write_text("// C header\n", encoding="utf-8")

    target_page = tmp_path / "docs" / "target.md"
    target_page.write_text(
        "# Target Page\n\n"
        "## Sub Section\n\n"
        "Explicit target {#explicit-id}\n\n"
        '<a id="html-id"></a>\n',
        encoding="utf-8",
    )

    doc = docs_api / "index.md"
    doc.write_text(
        "# API Index\n\n"
        "[Good File Link](../target.md)\n"
        "[Good Anchor Link](../target.md#sub-section)\n"
        "[Explicit ID Link](../target.md#explicit-id)\n"
        "[HTML ID Link](../target.md#html-id)\n"
        "[Source File Link](../../core/include/libvmaf.h)\n"
        "[Source Dir Link](../../core/include/)\n"
        "[Self Heading Link](#api-index)\n",
        encoding="utf-8",
    )

    rc = main(["--root", str(tmp_path), str(doc)])
    assert rc == EXIT_SUCCESS
    out, _ = capsys.readouterr()
    assert "7 links checked in 1 files" in out


def test_negative(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Verify findings and exit 1 on missing files and anchors."""
    docs_api = tmp_path / "docs" / "api"
    docs_api.mkdir(parents=True)

    target_page = docs_api / "target.md"
    target_page.write_text("# Target Page\n\n## Existing Section\n", encoding="utf-8")

    doc = docs_api / "bad.md"
    doc.write_text(
        "# Bad Links\n\n"
        "[Missing File](nonexistent.md)\n"
        "[Missing Anchor](target.md#no-such-section)\n"
        "[Missing Self Anchor](#no-such-self)\n",
        encoding="utf-8",
    )

    rc = main(["--root", str(tmp_path), str(doc)])
    assert rc == EXIT_FINDINGS
    out, err = capsys.readouterr()
    output = out + err
    assert "bad.md:3: missing target nonexistent.md" in output
    assert "bad.md:4: missing anchor target.md#no-such-section" in output
    assert "bad.md:5: missing anchor #no-such-self" in output


def test_boundary(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Verify code fences, external schemes, and duplicate heading slug deduplication."""
    docs_api = tmp_path / "docs" / "api"
    docs_api.mkdir(parents=True)

    target_page = docs_api / "dup.md"
    target_page.write_text(
        "# Overview\n\n"
        "## Duplicate Title\n"
        "First occurrence.\n\n"
        "## Duplicate Title\n"
        "Second occurrence.\n",
        encoding="utf-8",
    )

    doc = docs_api / "boundary.md"
    doc.write_text(
        "# Boundary Cases\n\n"
        "Link to repeated heading slug: [Second](dup.md#duplicate-title_1)\n"
        "Ignored scheme http: [HTTP](http://example.com)\n"
        "Ignored scheme https: [HTTPS](https://example.com)\n"
        "Ignored scheme mailto: [Mail](mailto:user@example.com)\n\n"
        "```markdown\n"
        "[Dead fenced link](missing_fenced.md#bad)\n"
        "```\n\n"
        "~~~text\n"
        "[Dead tilde fenced link](missing_tilde.md#bad)\n"
        "~~~\n\n"
        "Inline code link ignored: `[Dead inline link](missing_inline.md#bad)`\n",
        encoding="utf-8",
    )

    rc = main(["--root", str(tmp_path), str(doc)])
    assert rc == EXIT_SUCCESS
    out, _ = capsys.readouterr()
    assert "4 links checked in 1 files" in out


def test_duplicate_heading_hyphen_suffix_refused(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Verify a repeated heading is not reachable as "-1", which mkdocs never emits."""
    docs_api = tmp_path / "docs" / "api"
    docs_api.mkdir(parents=True)
    (docs_api / "dup.md").write_text("## Same\n\n## Same\n", encoding="utf-8")
    doc = docs_api / "index.md"
    doc.write_text("[Second](dup.md#same-1)\n", encoding="utf-8")

    rc = main(["--root", str(tmp_path), str(doc)])
    assert rc == EXIT_FINDINGS
    out, _ = capsys.readouterr()
    assert "missing anchor dup.md#same-1" in out


def test_unreadable_input(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Verify exit 2 on nonexistent or unreadable input."""
    nonexistent = tmp_path / "does_not_exist"
    rc = main(["--root", str(tmp_path), str(nonexistent)])
    assert rc == EXIT_UNREADABLE
    _, err = capsys.readouterr()
    assert "check_doc_links:" in err
