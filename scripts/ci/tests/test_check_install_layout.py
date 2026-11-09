#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Install-layout positive, negative and boundary checks; optional real SDK smoke."""

from __future__ import annotations

import collections.abc
import os
import shutil
import subprocess  # argv-list calls only, no shell  # nosec B404
import sys
from pathlib import Path, PureWindowsPath

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_install_layout as checker

from scripts.lib.safe_subprocess import run as run_command

CC_TIMEOUT_S = 60
CMAKE_TIMEOUT_S = 60
SUBPROCESS_TIMEOUT_S = 60

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


@pytest.mark.parametrize("suffix", ["", "@@WRONG_NODE", "@WRONG_NODE"])  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
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


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
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


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
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


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
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


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
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


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
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


def _configure_cli_test_env(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    consumers_callback: collections.abc.Callable[..., None] | None = None,
) -> None:
    (tmp_path / "meson-private").mkdir(parents=True, exist_ok=True)
    (tmp_path / "meson-private/coredata.dat").touch()
    monkeypatch.setattr(
        checker, "install_paths", lambda build, stage: (stage / "lib", stage / "include")
    )
    monkeypatch.setattr(checker, "check_tree", lambda *args: {"libvmafx": tmp_path / "engine.so"})
    monkeypatch.setattr(checker, "listed_symbols", lambda path: EXPECTED)
    monkeypatch.setattr(checker, "external_runtime_dirs", lambda build, installed: [])
    monkeypatch.setattr(
        checker,
        "check_consumers",
        consumers_callback if consumers_callback is not None else (lambda *args: None),
    )

    def fake_run(argv: list[str], env: dict[str, str] | None = None) -> str:
        if argv[0] == "meson":
            assert "--no-rebuild" in argv
            return ""
        return VERSIONS if "-V" in argv else SYMBOLS

    monkeypatch.setattr(checker, "run", fake_run)


def test_cli_unversioned_mutation_fails_and_is_reverted(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Exercise the same CLI path with captured-tool fixtures, then restore it."""
    consumers: list[bool] = []
    _configure_cli_test_env(
        monkeypatch, tmp_path, consumers_callback=lambda *args: consumers.append(True)
    )
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


def test_cmake_package_files_positive(tmp_path: Path) -> None:
    cmake_dir = tmp_path / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfig.cmake").touch()
    (cmake_dir / "vmafxConfigVersion.cmake").touch()
    checker.check_cmake_files(tmp_path / "lib", tmp_path)


def test_cmake_package_files_config_missing_fails(tmp_path: Path) -> None:
    cmake_dir = tmp_path / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfigVersion.cmake").touch()
    with pytest.raises(
        checker.LayoutError, match=r"missing CMake package file.*vmafxConfig\.cmake"
    ):
        checker.check_cmake_files(tmp_path / "lib", tmp_path)


def test_cmake_package_files_version_missing_fails(tmp_path: Path) -> None:
    cmake_dir = tmp_path / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfig.cmake").touch()
    with pytest.raises(
        checker.LayoutError, match=r"missing CMake package file.*vmafxConfigVersion\.cmake"
    ):
        checker.check_cmake_files(tmp_path / "lib", tmp_path)


def test_cmake_package_file_escapes_destdir_fails(tmp_path: Path) -> None:
    stage = tmp_path / "stage"
    stage.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "vmafxConfig.cmake").touch()
    cmake_dir = stage / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfig.cmake").symlink_to(outside / "vmafxConfig.cmake")
    (cmake_dir / "vmafxConfigVersion.cmake").touch()
    with pytest.raises(checker.LayoutError, match="escapes DESTDIR"):
        checker.check_cmake_files(stage / "lib", stage)


def _cmake_installed_map(
    build: Path,
    include_cmake: bool = True,
    only_version: bool = False,
) -> None:
    (build / "meson-info").mkdir(parents=True, exist_ok=True)
    mapping = {str(build / "src/libvmafx.so.1"): "/usr/lib/libvmafx.so.1"}
    if only_version:
        mapping[str(build / "vmafxConfigVersion.cmake")] = (
            "/usr/lib/cmake/vmafx/vmafxConfigVersion.cmake"
        )
    elif include_cmake:
        mapping[str(build / "vmafxConfig.cmake")] = "/usr/lib/cmake/vmafx/vmafxConfig.cmake"
        mapping[str(build / "vmafxConfigVersion.cmake")] = (
            "/usr/lib/cmake/vmafx/vmafxConfigVersion.cmake"
        )
    (build / "meson-info/intro-installed.json").write_text(checker.json.dumps(mapping))


def test_cmake_files_planned(tmp_path: Path) -> None:
    assert checker.cmake_files_planned(tmp_path) is False
    _cmake_installed_map(tmp_path, include_cmake=False)
    assert checker.cmake_files_planned(tmp_path) is False
    _cmake_installed_map(tmp_path, include_cmake=True)
    assert checker.cmake_files_planned(tmp_path) is True


def test_cmake_files_planned_version_only_triggers_layout_check(tmp_path: Path) -> None:
    build = tmp_path / "build"
    _cmake_installed_map(build, only_version=True)
    assert checker.cmake_files_planned(build) is True
    stage = tmp_path / "stage"
    cmake_dir = stage / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfigVersion.cmake").touch()
    with pytest.raises(
        checker.LayoutError, match=r"missing CMake package file.*vmafxConfig\.cmake"
    ):
        checker.check_cmake_layout(build, stage / "lib", stage, require_cmake=False)


def test_cmake_layout_planned_and_present_passes(tmp_path: Path) -> None:
    build = tmp_path / "build"
    _cmake_installed_map(build, include_cmake=True)
    stage = tmp_path / "stage"
    cmake_dir = stage / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfig.cmake").touch()
    (cmake_dir / "vmafxConfigVersion.cmake").touch()
    checker.check_cmake_layout(build, stage / "lib", stage, require_cmake=False)


def test_cmake_layout_planned_config_missing_fails(tmp_path: Path) -> None:
    build = tmp_path / "build"
    _cmake_installed_map(build, include_cmake=True)
    stage = tmp_path / "stage"
    cmake_dir = stage / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "vmafxConfigVersion.cmake").touch()
    with pytest.raises(
        checker.LayoutError, match=r"missing CMake package file.*vmafxConfig\.cmake"
    ):
        checker.check_cmake_layout(build, stage / "lib", stage, require_cmake=False)


def test_cmake_layout_not_planned_absent_passes(tmp_path: Path) -> None:
    build = tmp_path / "build"
    _cmake_installed_map(build, include_cmake=False)
    stage = tmp_path / "stage"
    stage.mkdir()
    checker.check_cmake_layout(build, stage / "lib", stage, require_cmake=False)


def test_cmake_layout_require_cmake_absent_fails(tmp_path: Path) -> None:
    build = tmp_path / "build"
    _cmake_installed_map(build, include_cmake=False)
    stage = tmp_path / "stage"
    stage.mkdir()
    with pytest.raises(checker.LayoutError, match="missing CMake package file"):
        checker.check_cmake_layout(build, stage / "lib", stage, require_cmake=True)


def test_cli_require_cmake_flag(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    _cmake_installed_map(tmp_path, include_cmake=False)
    _configure_cli_test_env(monkeypatch, tmp_path)
    assert checker.main(["--build", str(tmp_path)]) == 0
    assert checker.main(["--build", str(tmp_path), "--require-cmake"]) == 1


def _render_cmake_config(
    template_path: Path,
    output_path: Path,
    includedir: str,
    libdir: str,
    bindir: str = "bin",
    relative_depth: str = "../../..",
) -> None:
    template = template_path.read_text(encoding="utf-8")
    pkg_init = (
        f'get_filename_component(PACKAGE_PREFIX_DIR "${{CMAKE_CURRENT_LIST_DIR}}/{relative_depth}" ABSOLUTE)\n'
        "macro(set_and_check _var _file)\n"
        '  set(${_var} "${_file}")\n'
        '  if(NOT EXISTS "${_file}")\n'
        '    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")\n'
        "  endif()\n"
        "endmacro()\n"
    )
    rendered = template.replace("@PACKAGE_INIT@", pkg_init)
    rendered = rendered.replace("@includedir@", includedir)
    rendered = rendered.replace("@libdir@", libdir)
    rendered = rendered.replace("@bindir@", bindir)
    output_path.write_text(rendered, encoding="utf-8")


def _tool(name: str) -> str:
    """Absolute path of a build tool, or skip the test when it is absent."""
    path = shutil.which(name)
    if path is None:
        raise pytest.skip.Exception(f"{name} not available")
    return path


def _write_dummy_vmafx_shared(lib_dir: Path) -> Path:
    """Guards CMake packaging rules by building a minimal shared libvmafx.so exporting vmafx_version_string."""
    lib_dir.mkdir(parents=True, exist_ok=True)
    c_source = lib_dir / "dummy_vmafx.c"
    c_source.write_text('const char *vmafx_version_string(void) { return "1.0.0"; }\n')
    so_path = lib_dir / "libvmafx.so"
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [_tool("cc"), "-shared", "-fPIC", str(c_source), "-o", str(so_path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CC_TIMEOUT_S,
    )
    return so_path


def _write_cmake_version_file(dest: Path, version: str = "1.0.0") -> None:
    dest.write_text(
        f'set(PACKAGE_VERSION "{version}")\n'
        "set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
        "set(PACKAGE_VERSION_EXACT TRUE)\n",
        encoding="utf-8",
    )


def test_cmake_consumer_find_package_e2e(tmp_path: Path) -> None:
    """Guards core/cmake/vmafxConfig.cmake.in: verifies find_package(vmafx 1 REQUIRED) links and runs end-to-end."""
    if not shutil.which("cmake") or not shutil.which("cc"):
        pytest.skip("cmake or cc not available")
    prefix = tmp_path / "install"
    inc = prefix / "include" / "vmafx"
    inc.mkdir(parents=True)
    (inc / "vmafx.h").write_text("const char *vmafx_version_string(void);\n")
    lib = prefix / "lib"
    _write_dummy_vmafx_shared(lib)
    cmake_dir = lib / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    template = Path(__file__).resolve().parents[3] / "core" / "cmake" / "vmafxConfig.cmake.in"
    _render_cmake_config(template, cmake_dir / "vmafxConfig.cmake", "include", "lib")
    _write_cmake_version_file(cmake_dir / "vmafxConfigVersion.cmake")

    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(consumer_test C)\n"
        "find_package(vmafx 1 REQUIRED)\nadd_executable(app main.c)\n"
        "target_link_libraries(app PRIVATE vmafx::vmafx)\n"
    )
    (consumer / "main.c").write_text(
        "#include <vmafx/vmafx.h>\n#include <stdio.h>\n"
        "int main(void) { puts(vmafx_version_string()); return 0; }\n"
    )
    build_dir = consumer / "build"
    cmake_bin = _tool("cmake")
    run_command(
        [cmake_bin, "-S", str(consumer), "-B", str(build_dir), f"-DCMAKE_PREFIX_PATH={prefix}"],
        allowed_executables=(cmake_bin,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=CMAKE_TIMEOUT_S,
    )
    run_command(
        [cmake_bin, "--build", str(build_dir)],
        allowed_executables=(cmake_bin,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=CMAKE_TIMEOUT_S,
    )
    app_bin = str(build_dir / "app")
    out = run_command(
        [app_bin],
        allowed_executables=(app_bin,),
        env={**os.environ, "LD_LIBRARY_PATH": str(lib)},
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=SUBPROCESS_TIMEOUT_S,
    )
    assert out.stdout.strip() == "1.0.0"


def test_cmake_consumer_stale_cache_overridden(tmp_path: Path) -> None:
    """Guards core/cmake/vmafxConfig.cmake.in: ensures find_package overrides a stale VMAFX_LIBRARY cache entry with discovered targets."""
    if not shutil.which("cmake") or not shutil.which("cc"):
        pytest.skip("cmake or cc not available")
    prefix = tmp_path / "install"
    inc = prefix / "include" / "vmafx"
    inc.mkdir(parents=True)
    (inc / "vmafx.h").write_text("const char *vmafx_version_string(void);\n")
    lib = prefix / "lib"
    _write_dummy_vmafx_shared(lib)
    cmake_dir = lib / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    template = Path(__file__).resolve().parents[3] / "core" / "cmake" / "vmafxConfig.cmake.in"
    _render_cmake_config(template, cmake_dir / "vmafxConfig.cmake", "include", "lib")
    _write_cmake_version_file(cmake_dir / "vmafxConfigVersion.cmake")

    stale_lib = tmp_path / "stale_prefix" / "lib" / "libvmafx.so"
    stale_lib.parent.mkdir(parents=True)
    stale_lib.touch()

    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(consumer_test C)\n"
        "find_package(vmafx 1 REQUIRED)\n"
        "add_executable(app main.c)\n"
        "target_link_libraries(app PRIVATE vmafx::vmafx)\n"
    )
    (consumer / "main.c").write_text(
        "#include <vmafx/vmafx.h>\n#include <stdio.h>\nint main(void) { puts(vmafx_version_string()); return 0; }\n"
    )
    build_dir = consumer / "build"
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [
            _tool("cmake"),
            "-S",
            str(consumer),
            "-B",
            str(build_dir),
            f"-DCMAKE_PREFIX_PATH={prefix}",
            f"-DVMAFX_LIBRARY:FILEPATH={stale_lib}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CMAKE_TIMEOUT_S,
    )
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [_tool("cmake"), "--build", str(build_dir)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CMAKE_TIMEOUT_S,
    )
    out = subprocess.check_output(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [str(build_dir / "app")],
        env={**os.environ, "LD_LIBRARY_PATH": str(lib)},
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    assert out.strip() == "1.0.0"


def test_cmake_consumer_rejects_static_archive(tmp_path: Path) -> None:
    """Guards core/cmake/vmafxConfig.cmake.in: ensures find_package rejects a static archive when resolving shared imported targets."""
    if not shutil.which("cmake"):
        pytest.skip("cmake not available")
    prefix = tmp_path / "install"
    inc = prefix / "include" / "vmafx"
    inc.mkdir(parents=True)
    (inc / "vmafx.h").write_text("const char *vmafx_version_string(void);\n")
    lib = prefix / "lib"
    lib.mkdir(parents=True)
    (lib / "libvmafx.a").touch()
    cmake_dir = lib / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    template = Path(__file__).resolve().parents[3] / "core" / "cmake" / "vmafxConfig.cmake.in"
    _render_cmake_config(template, cmake_dir / "vmafxConfig.cmake", "include", "lib")
    _write_cmake_version_file(cmake_dir / "vmafxConfigVersion.cmake")

    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(consumer_test C)\n"
        "find_package(vmafx 1 REQUIRED)\n"
    )
    build_dir = consumer / "build"
    res = subprocess.run(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [
            _tool("cmake"),
            "-S",
            str(consumer),
            "-B",
            str(build_dir),
            f"-DCMAKE_PREFIX_PATH={prefix}",
        ],
        capture_output=True,
        text=True,
        timeout=CMAKE_TIMEOUT_S,
    )
    assert res.returncode != 0
    assert "Could NOT find vmafx (missing: VMAFX_LIBRARY)" in res.stderr


def test_cmake_consumer_absolute_dirs(tmp_path: Path) -> None:
    """Guards core/cmake/vmafxConfig.cmake.in: verifies find_package operates when prefix paths are configured as absolute paths."""
    if not shutil.which("cmake") or not shutil.which("cc"):
        pytest.skip("cmake or cc not available")
    abs_inc = tmp_path / "abs_include" / "vmafx"
    abs_inc.mkdir(parents=True)
    (abs_inc / "vmafx.h").write_text("const char *vmafx_version_string(void);\n")
    abs_lib = tmp_path / "abs_lib"
    _write_dummy_vmafx_shared(abs_lib)
    cmake_dir = tmp_path / "install" / "lib" / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    template = Path(__file__).resolve().parents[3] / "core" / "cmake" / "vmafxConfig.cmake.in"
    _render_cmake_config(
        template,
        cmake_dir / "vmafxConfig.cmake",
        str(abs_inc.parent),
        str(abs_lib),
    )
    _write_cmake_version_file(cmake_dir / "vmafxConfigVersion.cmake")

    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(consumer_test C)\n"
        "find_package(vmafx 1 REQUIRED)\n"
        "add_executable(app main.c)\n"
        "target_link_libraries(app PRIVATE vmafx::vmafx)\n"
    )
    (consumer / "main.c").write_text(
        "#include <vmafx/vmafx.h>\n#include <stdio.h>\nint main(void) { puts(vmafx_version_string()); return 0; }\n"
    )
    build_dir = consumer / "build"
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [
            _tool("cmake"),
            "-S",
            str(consumer),
            "-B",
            str(build_dir),
            f"-DCMAKE_PREFIX_PATH={tmp_path / 'install'}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CMAKE_TIMEOUT_S,
    )
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [_tool("cmake"), "--build", str(build_dir)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CMAKE_TIMEOUT_S,
    )
    out = subprocess.check_output(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [str(build_dir / "app")],
        env={**os.environ, "LD_LIBRARY_PATH": str(abs_lib)},
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    assert out.strip() == "1.0.0"


def test_cmake_consumer_mingw_import_library(tmp_path: Path) -> None:
    """Guards core/cmake/vmafxConfig.cmake.in: verifies MinGW layouts correctly populate IMPORTED_LOCATION and IMPORTED_IMPLIB targets."""
    if not shutil.which("cmake"):
        pytest.skip("cmake not available")
    prefix = tmp_path / "install"
    inc = prefix / "include" / "vmafx"
    inc.mkdir(parents=True)
    (inc / "vmafx.h").write_text("const char *vmafx_version_string(void);\n")
    lib = prefix / "lib"
    lib.mkdir(parents=True)
    (lib / "libvmafx.dll.a").touch()
    bin_dir = prefix / "bin"
    bin_dir.mkdir(parents=True)
    (bin_dir / "vmafx.dll").touch()
    cmake_dir = lib / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    template = Path(__file__).resolve().parents[3] / "core" / "cmake" / "vmafxConfig.cmake.in"
    _render_cmake_config(template, cmake_dir / "vmafxConfig.cmake", "include", "lib")
    _write_cmake_version_file(cmake_dir / "vmafxConfigVersion.cmake")

    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(consumer_mingw NONE)\n"
        "set(WIN32 TRUE)\n"
        "find_package(vmafx 1 REQUIRED)\n"
        "get_target_property(loc vmafx::vmafx IMPORTED_LOCATION)\n"
        "get_target_property(implib vmafx::vmafx IMPORTED_IMPLIB)\n"
        'file(WRITE "${CMAKE_BINARY_DIR}/target_props.txt" "${loc}\\n${implib}\\n")\n'
    )
    build_dir = consumer / "build"
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [
            _tool("cmake"),
            "-S",
            str(consumer),
            "-B",
            str(build_dir),
            f"-DCMAKE_PREFIX_PATH={prefix}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CMAKE_TIMEOUT_S,
    )
    props = (build_dir / "target_props.txt").read_text(encoding="utf-8").splitlines()
    assert Path(props[0]) == bin_dir / "vmafx.dll"
    assert Path(props[1]) == lib / "libvmafx.dll.a"


def test_cmake_consumer_stale_dll_cache_overridden(tmp_path: Path) -> None:
    """Guards core/cmake/vmafxConfig.cmake.in: ensures find_package overrides a stale cached DLL path on Windows/MinGW layouts."""
    if not shutil.which("cmake"):
        pytest.skip("cmake not available")
    prefix = tmp_path / "install"
    inc = prefix / "include" / "vmafx"
    inc.mkdir(parents=True)
    (inc / "vmafx.h").write_text("const char *vmafx_version_string(void);\n")
    lib = prefix / "lib"
    lib.mkdir(parents=True)
    (lib / "libvmafx.dll.a").touch()
    bin_dir = prefix / "bin"
    bin_dir.mkdir(parents=True)
    (bin_dir / "vmafx.dll").touch()
    cmake_dir = lib / "cmake" / "vmafx"
    cmake_dir.mkdir(parents=True)
    template = Path(__file__).resolve().parents[3] / "core" / "cmake" / "vmafxConfig.cmake.in"
    _render_cmake_config(template, cmake_dir / "vmafxConfig.cmake", "include", "lib")
    _write_cmake_version_file(cmake_dir / "vmafxConfigVersion.cmake")

    stale_dll = tmp_path / "stale_prefix" / "bin" / "vmafx.dll"
    stale_dll.parent.mkdir(parents=True)
    stale_dll.touch()

    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(consumer_stale_dll NONE)\n"
        "set(WIN32 TRUE)\n"
        "find_package(vmafx 1 REQUIRED)\n"
        "get_target_property(loc vmafx::vmafx IMPORTED_LOCATION)\n"
        'file(WRITE "${CMAKE_BINARY_DIR}/target_loc.txt" "${loc}\\n")\n'
    )
    build_dir = consumer / "build"
    subprocess.check_call(  # noqa: S603 -- resolved tool path or built test binary, argv list, no shell  # nosec B603
        [
            _tool("cmake"),
            "-S",
            str(consumer),
            "-B",
            str(build_dir),
            f"-DCMAKE_PREFIX_PATH={prefix}",
            f"-DVMAFX_DLL:FILEPATH={stale_dll}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=CMAKE_TIMEOUT_S,
    )
    loc = (build_dir / "target_loc.txt").read_text(encoding="utf-8").strip()
    assert Path(loc) == bin_dir / "vmafx.dll"


def test_cmake_target_props_windows_slash_parity() -> None:
    cmake_prop = "C:/install/bin/vmafx.dll"
    expected = PureWindowsPath(r"C:\install\bin\vmafx.dll")
    assert PureWindowsPath(cmake_prop) == expected
    assert cmake_prop != str(expected)


def test_meson_build_cmake_package_guards() -> None:
    meson_build = Path(__file__).resolve().parents[3] / "core" / "src" / "meson.build"
    content = meson_build.read_text(encoding="utf-8")
    assert "error('cmake_package requires a shared library" in content
    assert "install_dir: cmake_install_dir" in content
    assert "cmake_mod.configure_package_config_file(" in content
    assert "cmake_mod.write_basic_package_version_file(" in content
    options = Path(__file__).resolve().parents[3] / "core" / "meson_options.txt"
    options_content = options.read_text(encoding="utf-8")
    assert "option('cmake_package'" in options_content
    assert "type: 'feature'" in options_content


def test_real_install() -> None:
    build = os.environ.get("VMAF_BUILD_DIR")
    if not build:
        pytest.skip("set VMAF_BUILD_DIR to an already-built Linux shared-library Meson tree")
    assert checker.main(["--build", build]) == 0
