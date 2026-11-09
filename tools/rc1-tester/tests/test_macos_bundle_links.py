# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/check-macos-bundle-links.sh with fake `otool` and `file` on Linux."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
from pathlib import Path

import pytest

SCRIPT_TIMEOUT_S = 60

LINKS = Path(__file__).resolve().parents[3] / "scripts" / "ci" / "check-macos-bundle-links.sh"
pytestmark = pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")

SYSTEM = ["/usr/lib/libSystem.B.dylib", "/System/Library/Frameworks/Metal.framework/Metal"]


def script(path: Path, body: str) -> None:
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


class Bundle:
    """A fake bundle tree; `otool` answers from per-file canned text keyed by file name."""

    def __init__(self, tmp: Path) -> None:
        self.root = tmp / "bundle"
        self.root.mkdir()
        self.fake = tmp / "fake"
        self.fake.mkdir()
        script(
            self.fake / "file",
            'case "$2" in *.macho|*.dylib|*.so) echo "Mach-O 64-bit arm64";; *) echo text;; esac\n',
        )
        script(
            self.fake / "otool",
            f'f="$2"; b=$(basename "$f"); case "$1" in\n'
            f'  -L) cat "{self.fake}/$b.L";; -D) cat "{self.fake}/$b.D" 2>/dev/null || echo "$f:";;\n'
            f'  -l) cat "{self.fake}/$b.l" 2>/dev/null;;\nesac\n',
        )

    def add(self, rel, deps=(), ident=None, rpaths=()) -> Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
        name = path.name
        lines = [f"{path}:"] + ([f"\t{ident} (compat)"] if ident else [])
        lines += [f"\t{dep} (compat)" for dep in deps]
        (self.fake / f"{name}.L").write_text("\n".join(lines) + "\n")
        (self.fake / f"{name}.D").write_text(f"{path}:\n" + (f"{ident}\n" if ident else ""))
        load = "".join(
            f"Load command 9\n          cmd LC_RPATH\n      cmdsize 32\n         path {r} (offset 12)\n"
            for r in rpaths
        )
        (self.fake / f"{name}.l").write_text(load)
        return path

    def check(self) -> subprocess.CompletedProcess:
        env = {**os.environ, "PATH": f"{self.fake}:{os.environ['PATH']}"}
        return subprocess.run(
            ["bash", str(LINKS), str(self.root)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=SCRIPT_TIMEOUT_S,
        )


@pytest.fixture
def bundle(tmp_path: Path) -> Bundle:
    b = Bundle(tmp_path)
    b.add("build/tools/vmaf.macho", SYSTEM)
    b.add("tests/test_a.macho", SYSTEM)
    return b


def runtime(b: Bundle) -> None:
    """The interpreter layout of the real bundle: executable, libpython, extension module."""
    b.add("runtime/bin/python3.macho", [*SYSTEM, "@rpath/libpython3.13.dylib"],
          rpaths=["@executable_path/../lib"])  # fmt: skip
    b.add("runtime/lib/libpython3.13.dylib", [*SYSTEM],
          ident="@rpath/libpython3.13.dylib")  # fmt: skip
    b.add("runtime/lib/python3.13/lib-dynload/_ssl.so",
          [*SYSTEM, "@rpath/libpython3.13.dylib", "@loader_path/../../libpython3.13.dylib"],
          rpaths=["@loader_path/../.."])  # fmt: skip


def test_system_only_and_in_bundle_references_pass(bundle: Bundle) -> None:
    runtime(bundle)
    result = bundle.check()
    assert result.returncode == 0, result.stderr


def test_the_install_name_line_is_skipped(bundle: Bundle) -> None:
    # Without -D handling, libtcl's own `@rpath/libtcl9.0.dylib` and a bare ID both failed.
    bundle.add("runtime/lib/libtcl9.0.dylib", SYSTEM, ident="@rpath/libtcl9.0.dylib")
    bundle.add("runtime/lib/thread3.0.6/libthread3.0.6.dylib", SYSTEM, ident="libthread3.0.6.dylib")
    assert bundle.check().returncode == 0


def test_homebrew_dependency_fails_everywhere(bundle: Bundle) -> None:
    bundle.add("build/tools/vmaf.macho", [*SYSTEM, "/opt/homebrew/opt/libomp/lib/libomp.dylib"])
    result = bundle.check()
    assert result.returncode == 1 and "build/tools/vmaf.macho links /opt/homebrew" in result.stderr
    runtime_bundle = bundle.root / "runtime"
    bundle.add("runtime/lib/libx.dylib", [*SYSTEM, "/usr/local/lib/libfoo.dylib"])
    assert "runtime/lib/libx.dylib links /usr/local/lib/libfoo.dylib" in bundle.check().stderr
    assert runtime_bundle.exists()


def test_strict_files_accept_system_libraries_only(bundle: Bundle) -> None:
    runtime(bundle)
    bundle.add("tests/test_a.macho", [*SYSTEM, "@rpath/libpython3.13.dylib"],
               rpaths=["@loader_path/../runtime/lib"])  # fmt: skip
    result = bundle.check()
    assert (
        result.returncode == 1 and "tests/test_a.macho links @rpath/libpython3.13" in result.stderr
    )


def test_rpath_escaping_the_bundle_fails(bundle: Bundle, tmp_path: Path) -> None:
    (tmp_path / "outside").mkdir()
    (tmp_path / "outside" / "libleak.dylib").write_text("x")
    bundle.add("runtime/lib/libx.dylib", [*SYSTEM, "@rpath/libleak.dylib"],
               rpaths=["@loader_path/../../../outside"])  # fmt: skip
    result = bundle.check()
    assert result.returncode == 1 and "libx.dylib links @rpath/libleak.dylib" in result.stderr
    bundle.add("runtime/lib/libx.dylib", [*SYSTEM, "@loader_path/../../../outside/libleak.dylib"])
    assert bundle.check().returncode == 1


def test_unresolvable_reference_fails(bundle: Bundle) -> None:
    bundle.add("runtime/lib/libx.dylib", [*SYSTEM, "@rpath/libmissing.dylib"],
               rpaths=["@loader_path"])  # fmt: skip
    assert bundle.check().returncode == 1
    bundle.add("runtime/lib/libx.dylib", [*SYSTEM, "@rpath/libmissing.dylib"])  # no LC_RPATH at all
    assert bundle.check().returncode == 1


def test_rpath_chain_uses_the_first_entry_that_resolves(bundle: Bundle) -> None:
    bundle.add("runtime/lib/libreal.dylib", SYSTEM, ident="@rpath/libreal.dylib")
    bundle.add("runtime/lib/python3.13/lib-dynload/_m.so", [*SYSTEM, "@rpath/libreal.dylib"],
               rpaths=["@loader_path/nowhere", "/opt/homebrew/lib", "@loader_path/../.."])  # fmt: skip
    assert bundle.check().returncode == 0


def test_bare_name_resolves_next_to_the_file(bundle: Bundle) -> None:
    bundle.add("runtime/lib/itcl4/libitcl.dylib", SYSTEM)
    bundle.add("runtime/lib/itcl4/libtcl9itcl.dylib", [*SYSTEM, "libitcl.dylib"])
    assert bundle.check().returncode == 0
    bundle.add("runtime/lib/itcl4/libtcl9itcl.dylib", [*SYSTEM, "libother.dylib"])
    assert bundle.check().returncode == 1
