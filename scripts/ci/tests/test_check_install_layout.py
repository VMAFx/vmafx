#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Install-layout positive, negative and boundary checks; optional real SDK smoke."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_install_layout as checker

NODE = "VMAFX_TEST_1"
SYMBOL = "vmafx_version_string"
EXPECTED = {SYMBOL: NODE}
VERSIONS = f"""Version symbols section '.gnu.version' contains 3 entries:
  000:   0 (*local*)   1 (*global*)   2 ({NODE})
Version definition section '.gnu.version_d' contains 2 entries:
  000000: Rev: 1 Flags: BASE Index: 1 Cnt: 1 Name: libvmafx.so.test
  0x001c: Rev: 1 Flags: none Index: 2 Cnt: 1 Name: {NODE}
Version needs section '.gnu.version_r' contains 1 entry:
  0x0010: Name: GLIBC_TEST Flags: none Version: 3
"""
SYMBOLS = f"""Symbol table '.dynsym' contains 5 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND puts@GLIBC_TEST (3)
     2: 0000000000001000    20 FUNC    GLOBAL DEFAULT   12 {SYMBOL}@@{NODE}
     3: 0000000000000000     0 OBJECT  GLOBAL DEFAULT  ABS {NODE}
     4: 0000000000001020    10 FUNC    LOCAL  HIDDEN    12 private_function
"""


def test_positive_versioned_exports() -> None:
    exports = checker.parse_dynamic_symbols(SYMBOLS)
    definitions = checker.parse_version_definitions(VERSIONS)
    assert exports[SYMBOL] == NODE
    assert "puts" not in exports
    assert "private_function" not in exports
    assert "GLIBC_TEST" not in definitions
    checker.check_symbol_versions(exports, definitions, EXPECTED)


@pytest.mark.parametrize("suffix", ["", "@@WRONG_NODE", "@WRONG_NODE"])
def test_unversioned_or_wrong_node_fails(suffix: str) -> None:
    mutated = SYMBOLS.replace(f"{SYMBOL}@@{NODE}", f"{SYMBOL}{suffix}")
    with pytest.raises(checker.LayoutError, match="exported in"):
        checker.check_symbol_versions(checker.parse_dynamic_symbols(mutated), {NODE}, EXPECTED)


def test_node_in_needs_does_not_count_as_definition() -> None:
    imported = VERSIONS.replace(f"Name: {NODE}\n", "Name: OTHER_NODE\n")
    imported += f"  0x0020: Name: {NODE} Flags: none Version: 4\n"
    with pytest.raises(checker.LayoutError, match="missing version definition"):
        checker.check_symbol_versions(
            checker.parse_dynamic_symbols(SYMBOLS),
            checker.parse_version_definitions(imported),
            EXPECTED,
        )


@pytest.mark.parametrize(
    "text",
    [
        "",
        "No version information found in this file.",
        f"Version needs section '.gnu.version_r':\n  Name: {NODE}\n",
    ],
)
def test_missing_version_definitions_fail(text: str) -> None:
    with pytest.raises(checker.LayoutError, match="no version definitions"):
        checker.parse_version_definitions(text)


@pytest.mark.parametrize(
    "text",
    [
        "",
        "Symbol table '.dynsym' contains 0 entries:",
        "  1: broken",
        SYMBOLS.replace("GLOBAL", "LOCAL"),
    ],
)
def test_missing_or_malformed_symbols_fail(text: str) -> None:
    with pytest.raises(checker.LayoutError):
        checker.parse_dynamic_symbols(text)


def test_single_at_and_protected_weak_symbol() -> None:
    text = SYMBOLS.replace(f"@@{NODE}", f"@{NODE}").replace("GLOBAL DEFAULT", "WEAK PROTECTED")
    checker.check_symbol_versions(checker.parse_dynamic_symbols(text), {NODE}, EXPECTED)


def test_missing_and_extra_api_symbols_fail() -> None:
    with pytest.raises(checker.LayoutError, match="not exported"):
        checker.check_symbol_versions({}, {NODE}, EXPECTED)
    with pytest.raises(checker.LayoutError, match="missing from the VMAFx symbol list"):
        checker.check_symbol_versions({**EXPECTED, "vmafx_unlisted": NODE}, {NODE}, EXPECTED)


def test_duplicate_symbol_fails() -> None:
    with pytest.raises(checker.LayoutError, match="duplicate dynamic symbol"):
        checker.parse_dynamic_symbols(
            SYMBOLS + f"  5: 00001000 20 FUNC GLOBAL DEFAULT 12 {SYMBOL}\n"
        )


def test_soname_positive() -> None:
    assert (
        checker.parse_soname(" 0x000e (SONAME) Library soname: [libexample.so.42]\n")
        == "libexample.so.42"
    )


@pytest.mark.parametrize(
    "text",
    [
        "",
        " 0x0001 (NEEDED) Shared library: [libexample.so.42]",
        "(SONAME) Library soname: [one]\n(SONAME) Library soname: [two]",
    ],
)
def test_missing_or_duplicate_soname_fails(text: str) -> None:
    with pytest.raises(checker.LayoutError, match="exactly one ELF SONAME"):
        checker.parse_soname(text)


@pytest.mark.parametrize(
    "text",
    [
        "Requires: libvmafx",
        "Requires.private: libvmafx >= 1.0",
        "Requires: other, libvmafx >= 1.0",
        "Requires: other \\\n libvmafx",
    ],
)
def test_pkg_config_dependency_positive(text: str) -> None:
    assert checker.requires_vmafx(text)


@pytest.mark.parametrize(
    "text",
    [
        "",
        "Requires: libvmafx-other",
        "Requires: other-libvmafx",
        "Libs: -lvmafx",
        "# Requires: libvmafx",
        "Requires: other # libvmafx",
    ],
)
def test_pkg_config_missing_or_similar_name(text: str) -> None:
    assert not checker.requires_vmafx(text)


def test_staged_absolute_and_relative_install_directories(tmp_path: Path) -> None:
    assert (
        checker.staged_path(tmp_path, "/opt/sdk", "lib/triplet") == tmp_path / "opt/sdk/lib/triplet"
    )
    assert checker.staged_path(tmp_path, "/opt/sdk", "/usr/include") == tmp_path / "usr/include"
    with pytest.raises(checker.LayoutError, match="escapes DESTDIR"):
        checker.staged_path(tmp_path, "/usr", "../../../../etc")


def test_environment_excludes_host_sdk(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setenv("PKG_CONFIG_PATH", "/host/pkgconfig")
    monkeypatch.setenv("PKG_CONFIG_SYSROOT_DIR", "/host")
    monkeypatch.setenv("CPATH", "/host/include")
    monkeypatch.setenv("LD_LIBRARY_PATH", "/host/lib")
    libdir = tmp_path / "usr/lib/triplet"
    env = checker.consumer_environment(tmp_path, libdir)
    assert env["PKG_CONFIG_PATH"] == env["PKG_CONFIG_LIBDIR"] == str(libdir / "pkgconfig")
    assert env["PKG_CONFIG_SYSROOT_DIR"] == str(tmp_path)
    assert env["LD_LIBRARY_PATH"] == str(libdir)
    assert "CPATH" not in env


def test_cli_unversioned_mutation_fails_and_is_reverted(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Exercise the same CLI path with captured-tool fixtures, then restore it."""
    (tmp_path / "meson-private").mkdir()
    (tmp_path / "meson-private/coredata.dat").touch()
    monkeypatch.setattr(
        checker, "install_paths", lambda build, stage: (stage / "lib", stage / "include")
    )
    monkeypatch.setattr(checker, "check_tree", lambda *args: {"libvmafx": tmp_path / "engine.so"})
    monkeypatch.setattr(checker, "listed_symbols", lambda path: EXPECTED)
    monkeypatch.setattr(checker, "external_runtime_dirs", lambda build, installed: [])
    consumers: list[bool] = []
    monkeypatch.setattr(checker, "check_consumers", lambda *args: consumers.append(True))

    def fake_run(argv: list[str], env: dict[str, str] | None = None) -> str:
        if argv[0] == "meson":
            assert "--no-rebuild" in argv
            return ""
        return VERSIONS if "-V" in argv else SYMBOLS

    monkeypatch.setattr(checker, "run", fake_run)
    assert checker.main(["--build", str(tmp_path)]) == 0
    assert checker.main(["--build", str(tmp_path), "--plant-unversioned"]) == 1
    assert "no version node" in capsys.readouterr().err
    assert checker.main(["--build", str(tmp_path)]) == 0
    assert consumers == [True, True]


def test_consumers_use_staged_flags_and_run_both_binaries(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    libdir = tmp_path / "lib with spaces"
    include = tmp_path / "include with spaces"
    calls: list[list[str]] = []
    monkeypatch.setenv("CC", "cc -std=c11")

    def fake_run(argv: list[str], env: dict[str, str] | None = None) -> str:
        assert env is not None
        assert env["PKG_CONFIG_SYSROOT_DIR"] == str(tmp_path)
        assert env["LD_LIBRARY_PATH"] == str(libdir)
        calls.append(argv)
        if argv[0] == "pkg-config":
            return f"'-I{include}' '-L{libdir}' -l{argv[-1].removeprefix('lib')}"
        return "consumer-version" if len(argv) == 1 else ""

    monkeypatch.setattr(checker, "run", fake_run)
    checker.check_consumers(tmp_path, libdir, tmp_path)
    for package in ("libvmafx", "libvmaf"):
        binary = str(tmp_path / f"{package}-consumer")
        assert ["pkg-config", "--cflags", "--libs", package] in calls
        assert [binary] in calls
        compile_call = next(call for call in calls if call[0] == "cc" and binary in call)
        assert f"-I{include}" in compile_call
        assert f"-L{libdir}" in compile_call
        assert "-std=c11" in compile_call
        assert f"-l{package.removeprefix('lib')}" in compile_call
    assert "vmafx_version_string()" in (tmp_path / "libvmafx-consumer.c").read_text()
    assert "vmaf_version()" in (tmp_path / "libvmaf-consumer.c").read_text()


def test_runpath_parsing_positive_and_empty() -> None:
    text = (
        " 0x1d (RUNPATH)            Library runpath: [/opt/ort/lib64::$ORIGIN/x]\n"
        " 0x0f (RPATH)              Library rpath: [/opt/other]\n"
    )
    assert checker.parse_runpath(text) == ["/opt/ort/lib64", "$ORIGIN/x", "/opt/other"]
    assert checker.parse_runpath(" 0x1 (NEEDED) Shared library: [libm.so.6]\n") == []


def _installed_map(build: Path, name: str, count: int = 1) -> None:
    (build / "meson-info").mkdir(parents=True)
    mapping = {str(build / f"src{index}" / name): f"/usr/lib/{name}" for index in range(count)}
    (build / "meson-info/intro-installed.json").write_text(checker.json.dumps(mapping))


def test_external_runtime_dirs_keeps_outside_drops_build_and_origin(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    build = tmp_path / "build"
    outside = tmp_path / "ort"
    outside.mkdir()
    _installed_map(build, "libvmafx.so.1")
    runpath = f"{outside}:{build / 'src'}:$ORIGIN:relative"
    monkeypatch.setattr(
        checker, "run", lambda argv, env=None: f" (RUNPATH) Library runpath: [{runpath}]\n"
    )
    assert checker.external_runtime_dirs(build, tmp_path / "libvmafx.so.1") == [str(outside)]


def test_external_runtime_dirs_refuses_vmafx_library_and_ambiguous_source(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    build = tmp_path / "build"
    outside = tmp_path / "stale"
    outside.mkdir()
    (outside / "libvmafx.so.1").touch()
    _installed_map(build, "libvmafx.so.1")
    monkeypatch.setattr(
        checker, "run", lambda argv, env=None: f" (RUNPATH) Library runpath: [{outside}]\n"
    )
    with pytest.raises(checker.LayoutError, match="holds a VMAFx library"):
        checker.external_runtime_dirs(build, tmp_path / "libvmafx.so.1")
    other = tmp_path / "other"
    _installed_map(other, "libvmafx.so.1", count=2)
    with pytest.raises(checker.LayoutError, match="no single build file"):
        checker.external_runtime_dirs(other, tmp_path / "libvmafx.so.1")


def test_real_install() -> None:
    build = os.environ.get("VMAF_BUILD_DIR")
    if not build:
        pytest.skip("set VMAF_BUILD_DIR to an already-built Linux shared-library Meson tree")
    assert checker.main(["--build", build]) == 0
