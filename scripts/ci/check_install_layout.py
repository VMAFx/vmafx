#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check a Linux ELF installation and run consumers using only its staged SDK.

Requires an already-built shared-library Meson tree, readelf, pkg-config and
a C compiler (CC, default cc). Exit 0: pass; exit 1: finding or tool failure.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "core" / "test"))
from check_exported_symbols import (  # noqa: E402 -- reuse the existing export policy
    DynamicSymbols,
    listed_symbols,
    split_version,
    vmafx_findings,
)

COMMAND_TIMEOUT = 300


class LayoutError(RuntimeError):
    """The staged installation does not satisfy the install contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise LayoutError(message)


def run(argv: list[str], env: dict[str, str] | None = None) -> str:
    """Run bounded, shell-free commands; preserve diagnostics on failure."""
    executable = shutil.which(argv[0])
    require(executable is not None, f"required tool not found: {argv[0]}")
    env = {**(os.environ if env is None else env), "LC_ALL": "C"}
    result = subprocess.run(  # noqa: S603 -- resolved executable, argv only, no shell
        [str(executable), *argv[1:]],
        env=env,
        check=False,
        capture_output=True,
        text=True,
        timeout=COMMAND_TIMEOUT,
    )
    require(
        result.returncode == 0,
        f"{shlex.join(argv)} exited {result.returncode}\n{result.stdout}{result.stderr}",
    )
    return result.stdout


def parse_soname(text: str) -> str:
    """Read the unique DT_SONAME from readelf -d output."""
    names = re.findall(r"\(SONAME\)\s+Library soname:\s*\[([^\]]+)\]", text)
    require(len(names) == 1, "expected exactly one ELF SONAME")
    return names[0]


def parse_version_definitions(text: str) -> set[str]:
    """Read definitions, excluding nodes merely imported by version needs."""
    definitions: set[str] = set()
    in_definitions = False
    for line in text.splitlines():
        if line.startswith("Version "):
            in_definitions = line.startswith("Version definition section ")
        if in_definitions and (match := re.search(r"\bName:\s+(\S+)", line)):
            definitions.add(match[1])
    require(bool(definitions), "ELF has no version definitions")
    return definitions


def parse_dynamic_symbols(text: str) -> dict[str, str | None]:
    """Read defined, externally visible symbols from readelf --dyn-syms -W."""
    exports: dict[str, str | None] = {}
    for line in text.splitlines():
        fields = line.split()
        if not fields or not re.fullmatch(r"\d+:", fields[0]):
            continue
        require(len(fields) >= 7, f"malformed dynamic symbol: {line}")  # noqa: PLR2004
        if fields[6] == "UND" or fields[4] not in {"GLOBAL", "WEAK", "UNIQUE"}:
            continue
        if fields[5] not in {"DEFAULT", "PROTECTED"}:
            continue
        require(len(fields) >= 8, f"dynamic symbol lacks a name: {line}")  # noqa: PLR2004
        name, node = split_version(fields[7])
        require(name not in exports, f"duplicate dynamic symbol: {name}")
        exports[name] = node
    require(bool(exports), "ELF has no visible dynamic exports")
    return exports


def check_symbol_versions(
    exports: dict[str, str | None], definitions: set[str], expected: dict[str, str]
) -> None:
    """Apply the generated export policy and require locally defined nodes."""
    findings = vmafx_findings(DynamicSymbols(exports, versions_visible=True), expected)
    missing = set(expected.values()) - definitions
    findings.extend(f"missing version definition: {node}" for node in sorted(missing))
    require(not findings, "\n".join(findings))


def requires_vmafx(text: str) -> bool:
    """Accept a whole package name in Requires or Requires.private, with constraints."""
    logical = text.replace("\\\n", " ")
    for line in logical.splitlines():
        key, _, value = line.partition(":")
        if key.strip() in {"Requires", "Requires.private"}:
            packages = re.findall(r"(?:^|[,\s])([A-Za-z_][\w.+-]*)", value.split("#", 1)[0])
            if "libvmafx" in packages:
                return True
    return False


def expected_sonames() -> dict[str, str]:
    """SONAME majors belong to Meson, independently of product/API versions."""
    source = (ROOT / "core" / "meson.build").read_text(encoding="utf-8")
    names: dict[str, str] = {}
    for package in ("libvmafx", "libvmaf"):
        variable = package.removeprefix("lib") + "_soname_version"
        match = re.search(rf"^{variable}\s*=\s*'([\d.]+)'", source, re.MULTILINE)
        require(match is not None, f"Meson does not declare {variable}")
        major = match[1].split(".")[0]
        names[package] = f"{package}.so.{major}"
    return names


def staged_path(stage: Path, prefix: str, directory: str) -> Path:
    installed = Path(directory)
    if not installed.is_absolute():
        installed = Path(prefix) / installed
    require(installed.is_absolute(), f"install directory is not absolute: {installed}")
    path = stage / installed.relative_to(installed.anchor)
    require(path.resolve().is_relative_to(stage), f"install directory escapes DESTDIR: {path}")
    return path


def install_paths(build: Path, stage: Path) -> tuple[Path, Path]:
    options = json.loads((build / "meson-info" / "intro-buildoptions.json").read_text())
    values = {option["name"]: option["value"] for option in options}
    return (
        staged_path(stage, values["prefix"], values["libdir"]),
        staged_path(stage, values["prefix"], values["includedir"]),
    )


def check_tree(libdir: Path, includedir: Path, stage: Path) -> dict[str, Path]:
    libraries: dict[str, Path] = {}
    for package, soname in expected_sonames().items():
        for name in (f"{package}.so", soname):
            path = libdir / name
            require(path.is_file(), f"missing installed library: {path}")
            require(path.resolve().is_relative_to(stage), f"library escapes DESTDIR: {path}")
        library = libdir / soname
        require(
            parse_soname(run(["readelf", "-d", str(library)])) == soname,
            f"wrong SONAME: {library}, expected {soname}",
        )
        pc = libdir / "pkgconfig" / f"{package}.pc"
        require(pc.is_file(), f"missing {package}.pc")
        require(pc.resolve().is_relative_to(stage), f"pkg-config file escapes DESTDIR: {pc}")
        libraries[package] = library
    for directory in ("vmafx", "libvmaf"):
        headers = includedir / directory
        require(headers.is_dir() and any(headers.glob("*.h")), f"missing headers: {headers}")
        require(headers.resolve().is_relative_to(stage), f"headers escape DESTDIR: {headers}")
    compat_pc = libdir / "pkgconfig" / "libvmaf.pc"
    require(
        requires_vmafx(compat_pc.read_text(encoding="utf-8")),
        "libvmaf.pc lacks Requires or Requires.private: libvmafx",
    )
    return libraries


def parse_runpath(text: str) -> list[str]:
    entries: list[str] = []
    for match in re.finditer(r"\((?:RUNPATH|RPATH)\)\s+Library r(?:un)?path: \[([^\]]*)\]", text):
        entries.extend(entry for entry in match.group(1).split(":") if entry)
    return entries


def external_runtime_dirs(build: Path, installed: Path) -> list[str]:
    """Return the build library's RUNPATH directories outside the build tree.

    Meson strips the build RUNPATH at install, so a dependency outside the
    default loader path (ONNX Runtime in a private prefix) is found through the
    build's own search path. No VMAFx library may come from these directories.
    """
    mapping = json.loads((build / "meson-info" / "intro-installed.json").read_text())
    name = installed.resolve().name
    sources = [Path(source) for source, target in mapping.items() if Path(target).name == name]
    require(len(sources) == 1, f"no single build file installs {name}")
    root = build.resolve()
    directories: list[str] = []
    for entry in parse_runpath(run(["readelf", "-d", str(sources[0])])):
        path = Path(entry)
        if "$" in entry or not path.is_absolute() or path.resolve().is_relative_to(root):
            continue
        require(not any(path.glob("libvmaf*.so*")), f"build RUNPATH holds a VMAFx library: {path}")
        directories.append(str(path))
    return directories


def consumer_environment(
    stage: Path, libdir: Path, external: tuple[str, ...] = ()
) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not key.startswith("PKG_CONFIG_")}
    for key in ("CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH", "LD_PRELOAD"):
        env.pop(key, None)
    pcdir = str(libdir / "pkgconfig")
    env.update(
        PKG_CONFIG_PATH=pcdir,
        PKG_CONFIG_LIBDIR=pcdir,
        PKG_CONFIG_SYSROOT_DIR=str(stage),
        PKG_CONFIG_DISABLE_UNINSTALLED="1",
        LD_LIBRARY_PATH=os.pathsep.join((str(libdir), *external)),
        LC_ALL="C",
    )
    return env


def check_consumers(stage: Path, libdir: Path, work: Path, external: tuple[str, ...] = ()) -> None:
    env = consumer_environment(stage, libdir, external)
    links = [f"-Wl,-rpath-link,{directory}" for directory in external]
    compiler = shlex.split(os.environ.get("CC", "cc"))
    require(bool(compiler), "CC is empty")
    for package, header, function in (
        ("libvmafx", "vmafx/version.h", "vmafx_version_string"),
        ("libvmaf", "libvmaf/libvmaf.h", "vmaf_version"),
    ):
        source = work / f"{package}-consumer.c"
        binary = work / f"{package}-consumer"
        source.write_text(
            f"#include <{header}>\n#include <stdio.h>\n"
            f"int main(void) {{ const char *v = {function}(); "
            "if (!v || !*v) return 1; return puts(v) < 0; }\n",
            encoding="utf-8",
        )
        flags = shlex.split(run(["pkg-config", "--cflags", "--libs", package], env))
        run([*compiler, str(source), "-o", str(binary), *flags, *links], env)
        version = run([str(binary)], env).strip()
        require(bool(version), f"{package} consumer returned an empty version")
        print(f"PASS: {package} installed consumer: {version}")


def check_install(build: Path, plant_unversioned: bool = False) -> None:
    require(
        (build / "meson-private" / "coredata.dat").is_file(),
        f"not a configured Meson build: {build}",
    )
    with tempfile.TemporaryDirectory(prefix="vmafx-install-layout-") as temporary:
        work = Path(temporary)
        stage = work / "destdir"
        stage.mkdir()
        # The caller builds with its own resource limits; installation never starts Ninja.
        run(["meson", "install", "--no-rebuild", "--destdir", str(stage), "-C", str(build)])
        libdir, includedir = install_paths(build, stage)
        libraries = check_tree(libdir, includedir, stage)
        engine = str(libraries["libvmafx"])
        definitions = parse_version_definitions(run(["readelf", "-V", engine]))
        exports = parse_dynamic_symbols(run(["readelf", "--dyn-syms", "--wide", engine]))
        if plant_unversioned:
            # Mutation confined to this inspection: the build/install remain untouched.
            exports["vmafx_version_string"] = None
        check_symbol_versions(
            exports, definitions, listed_symbols(ROOT / "core/src/vmafx_symbols.txt")
        )
        external = tuple(external_runtime_dirs(build, libraries["libvmafx"]))
        check_consumers(stage, libdir, work, external)
    print("PASS: installed libraries, headers, pkg-config, SONAMEs and version nodes")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build", type=Path, required=True, help="already-built Linux ELF Meson tree"
    )
    parser.add_argument(
        "--plant-unversioned",
        action="store_true",
        help="negative control: inspect the version function as unversioned",
    )
    args = parser.parse_args(argv)
    try:
        check_install(args.build.resolve(), args.plant_unversioned)
    except (LayoutError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
