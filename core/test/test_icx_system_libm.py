#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""An icx / icpx build takes its host math functions from the C library's libm.

ADR-1495. The Intel driver links its own math library, libimf, into every link
it runs, and an icx-built `vmaf` took a static copy of it and exported the
functions libvmaf.so needs, so `log10`, `pow`, `powf`, `log2f`, `exp` and `log`
resolved to Intel's implementations and the CPU extractors psnr, psnr_hvs,
ciede, adm and float_adm differed from a GCC build in the last digits
(T-ICX-LIBIMF-HOST-MATH-2026-10-01). `core/src/meson.build` passes
`-no-intel-lib=libimf` to every C and C++ link of an icx build.

This test reads the binaries of the build it runs in: libvmaf.so must not need
libimf.so and must import the six functions from glibc (versioned references),
must import no vectorised SVML math routine, and `vmaf` must define none of
them. Then the dynamic loader relocates `vmaf` and everything it loads with
every symbol bound, in its list mode (what `ldd -r` does: the program is not
run and no IFUNC resolver is called), and must bind each reference to the six
functions to libm.so.6. On a build whose C and C++ compilers are not Intel
LLVM the binary checks skip with the reason; the parser checks and the
loader-trace check run on every build.

Running the program with `LD_BIND_NOW=1` instead crashed a SYCL build on
Ubuntu (T-ICX-LIBM-TEST-BIND-NOW-CRASH-2026-10-04): the SYCL runtime loads
Intel's libimf.so, which binds `cosf` to libm.so.6's IFUNC without depending
on libm.so.6, so glibc relocated it first and called libm's resolver before
libm was relocated (`Relink ... for IFUNC symbol 'cosf'`, signal 11). glibc's
list mode relocates with `__RTLD_NOIFUNC` (elf/rtld.c), from the same search
list, so the bindings are the ones a run makes.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path
from unittest import mock

# The functions the row names; every CPU extractor that differed calls one.
MATH_FUNCTIONS = ("log10", "pow", "powf", "log2f", "exp", "log")
INTEL_LLVM = "intel-llvm"
# Set by the test() call in core/test/meson.build.
BUILD_ROOT_ENV = "VMAF_ICX_LIBM_BUILD_ROOT"
LIBRARY_ENV = "VMAF_ICX_LIBM_LIBRARY"
CLI_ENV = "VMAF_ICX_LIBM_CLI"

NEEDED = re.compile(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]")
# readelf -W --dyn-syms: "Num: Value Size Type Bind Vis Ndx Name [(verndx)]".
DYNSYM = re.compile(
    r"^\s*\d+:\s+[0-9a-fA-F]+\s+\S+\s+\w+\s+\w+\s+\w+\s+(\w+)\s+([^\s@]+)(?:@@?(\S+))?"
)
# glibc's LD_DEBUG=bindings: "binding file A [0] to B [0]: normal symbol `name'".
BINDING = re.compile(r"binding file (\S+) \[\d+\] to (\S+) \[\d+\]: \w+ symbol `([^']+)'")


def needed_libraries(dynamic_section: str) -> list[str]:
    """The DT_NEEDED entries of `readelf -d` output."""
    return NEEDED.findall(dynamic_section)


def dynamic_symbols(dynsym_table: str) -> dict[str, tuple[bool, str]]:
    """name -> (defined here, version) from `readelf -W --dyn-syms` output."""
    symbols: dict[str, tuple[bool, str]] = {}
    for line in dynsym_table.splitlines():
        match = DYNSYM.match(line)
        if match is not None and match.group(2):
            ndx, name, version = match.group(1), match.group(2), match.group(3) or ""
            symbols[name] = (ndx != "UND", version)
    return symbols


def library_failures(dynamic_section: str, dynsym_table: str) -> list[str]:
    """Why a linked libvmaf does not take the math functions from glibc."""
    failures: list[str] = []
    needed = needed_libraries(dynamic_section)
    for name in needed:
        if name.startswith("libimf"):
            failures.append(f"libvmaf needs {name}")
    if not any(name.startswith("libm.so") for name in needed):
        failures.append("libvmaf does not need libm.so")
    symbols = dynamic_symbols(dynsym_table)
    for function in MATH_FUNCTIONS:
        defined, version = symbols.get(function, (False, ""))
        if defined:
            failures.append(f"libvmaf defines {function}")
        elif not version.startswith("GLIBC_"):
            failures.append(
                f"libvmaf imports {function} without a glibc version ({version or 'none'})"
            )
    for name in symbols:
        if name.startswith("__svml_"):
            failures.append(f"libvmaf imports the vectorised math routine {name}")
    return failures


def cli_failures(dynamic_section: str, dynsym_table: str) -> list[str]:
    """Why the CLI would hand libvmaf another math library's functions."""
    failures = [
        f"vmaf needs {name}"
        for name in needed_libraries(dynamic_section)
        if name.startswith("libimf")
    ]
    symbols = dynamic_symbols(dynsym_table)
    for function in MATH_FUNCTIONS:
        if symbols.get(function, (False, ""))[0]:
            failures.append(f"vmaf defines and exports {function}")
    return failures


def binding_failures(loader_trace: str, referrers: tuple[str, ...]) -> list[str]:
    """Why the loader did not bind every reference to the functions to libm.so.

    `referrers` are the basenames of the objects whose references count (the
    library and the CLI); objects such as libimf.so binding to themselves do not.
    """
    bound: dict[str, set[str]] = {function: set() for function in MATH_FUNCTIONS}
    for match in BINDING.finditer(loader_trace):
        referrer, target, symbol = match.groups()
        if symbol in bound and Path(referrer).name.startswith(referrers):
            bound[symbol].add(Path(target).name)
    failures: list[str] = []
    for function, targets in bound.items():
        if not targets:
            failures.append(f"no reference to {function} was bound")
        for target in sorted(targets):
            if not target.startswith("libm.so"):
                failures.append(f"{function} was bound to {target}, not libm.so")
    return failures


def _readelf(*args: str) -> str:
    readelf = shutil.which("readelf")
    if readelf is None:
        raise AssertionError("readelf (binutils) is not installed")
    return subprocess.run(  # noqa: S603 -- readelf from PATH, fixed arguments.
        [readelf, *args], capture_output=True, check=True, encoding="utf-8", timeout=60
    ).stdout


def _loader_trace(cli: Path) -> str:
    """glibc's bindings for every relocation of `cli` and its libraries.

    LD_TRACE_LOADED_OBJECTS with LD_WARN relocates every object and runs
    nothing; LD_BIND_NOW makes it bind the function references too.
    """
    env = dict(
        os.environ,
        LD_TRACE_LOADED_OBJECTS="1",
        LD_WARN="1",
        LD_BIND_NOW="1",
        LD_DEBUG="bindings",
    )
    result = subprocess.run(  # noqa: S603 -- the CLI of this build.
        [str(cli)],
        capture_output=True,
        check=False,
        encoding="utf-8",
        env=env,
        timeout=60,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"loader trace of {cli} exited {result.returncode}:\n{result.stderr[-2000:]}"
        )
    return result.stderr


def _gnu_libc_version() -> str:
    """`glibc 2.xx`, or an empty string on a platform without glibc.

    `os.confstr("CS_GNU_LIBC_VERSION")` raises ValueError on macOS and has no
    such name on Windows, so it is guarded rather than trusted.
    """
    try:
        return os.confstr("CS_GNU_LIBC_VERSION") or ""
    except (AttributeError, ValueError, OSError):
        return ""


def _build_compiler_ids(build_root: Path) -> tuple[str, str]:
    compilers = json.loads(
        (build_root / "meson-info" / "intro-compilers.json").read_text(encoding="utf-8")
    )
    host = compilers["host"]
    return host["c"]["id"], host.get("cpp", {}).get("id", "")


SYMBOLS_GLIBC = """\
Symbol table '.dynsym' contains 8 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND log10@GLIBC_2.2.5 (2)
     2: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND powf@GLIBC_2.27 (3)
     3: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND exp@GLIBC_2.29 (4)
     4: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND log2f@GLIBC_2.27 (3)
     5: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND pow@GLIBC_2.29 (4)
     6: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND log@GLIBC_2.29 (4)
     7: 00000000000011b0   639 FUNC    GLOBAL DEFAULT   10 vmaf_init
"""
NEEDED_GLIBC = (
    " 0x01 (NEEDED) Shared library: [libm.so.6]\n 0x01 (NEEDED) Shared library: [libc.so.6]\n"
)
NEEDED_IMF = (
    " 0x01 (NEEDED) Shared library: [libimf.so]\n 0x01 (NEEDED) Shared library: [libc.so.6]\n"
)


class ParserTest(unittest.TestCase):
    """Positive, negative and boundary cases of the checks, on any build."""

    def test_glibc_library_passes(self) -> None:
        self.assertEqual(library_failures(NEEDED_GLIBC, SYMBOLS_GLIBC), [])

    def test_libimf_library_fails(self) -> None:
        # The library of an icx build before ADR-1495: libimf, no versions.
        unversioned = re.sub(r"@GLIBC_[\d.]+ \(\d\)", "", SYMBOLS_GLIBC)
        failures = library_failures(NEEDED_IMF, unversioned)
        self.assertIn("libvmaf needs libimf.so", failures)
        self.assertIn("libvmaf does not need libm.so", failures)
        self.assertIn("libvmaf imports pow without a glibc version (none)", failures)
        self.assertEqual(len(failures), 2 + len(MATH_FUNCTIONS))

    def test_svml_reference_and_missing_function_fail(self) -> None:
        planted = (
            SYMBOLS_GLIBC
            + "     8: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND __svml_powf8\n"
        )
        self.assertEqual(
            library_failures(NEEDED_GLIBC, planted),
            ["libvmaf imports the vectorised math routine __svml_powf8"],
        )
        # Boundary: a library that references a function under another name.
        renamed = SYMBOLS_GLIBC.replace(" log@GLIBC", " logl@GLIBC")
        self.assertEqual(
            library_failures(NEEDED_GLIBC, renamed),
            ["libvmaf imports log without a glibc version (none)"],
        )

    def test_cli_that_exports_libimf_copies_fails(self) -> None:
        exported = "   126: 0000000000411550    13 FUNC    GLOBAL DEFAULT   11 exp\n"
        self.assertEqual(cli_failures(NEEDED_GLIBC, exported), ["vmaf defines and exports exp"])
        self.assertEqual(cli_failures(NEEDED_IMF, ""), ["vmaf needs libimf.so"])
        # Boundary: a CLI that imports the function is what a fixed build has.
        imported = "   126: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND exp@GLIBC_2.29 (4)\n"
        self.assertEqual(cli_failures(NEEDED_GLIBC, imported), [])

    def test_loader_bindings(self) -> None:
        line = "  7:\tbinding file /b/src/libvmaf.so.3 [0] to {} [0]: normal symbol `{}' [GLIBC_2.29]\n"
        good = "".join(line.format("/usr/lib/libm.so.6", f) for f in MATH_FUNCTIONS)
        self.assertEqual(binding_failures(good, ("libvmaf", "vmaf")), [])
        # The icx build before ADR-1495: libvmaf's exp bound to the CLI's copy.
        bad = good.replace(
            "/usr/lib/libm.so.6 [0]: normal symbol `exp'", "tools/vmaf [0]: normal symbol `exp'"
        )
        self.assertEqual(
            binding_failures(bad, ("libvmaf", "vmaf")), ["exp was bound to vmaf, not libm.so"]
        )
        # libimf binding its own internals is not a reference of ours.
        noise = "  7:\tbinding file /opt/libimf.so [0] to /opt/libimf.so [0]: normal symbol `pow'\n"
        self.assertEqual(binding_failures(good + noise, ("libvmaf", "vmaf")), [])
        # Boundary: a trace without any binding (no glibc loader) is a failure.
        self.assertEqual(len(binding_failures("", ("libvmaf", "vmaf"))), len(MATH_FUNCTIONS))


class LibcDetectionTest(unittest.TestCase):
    """The glibc probe answers on every platform instead of raising."""

    def test_confstr_value_error_means_no_glibc(self) -> None:
        # macOS: os.confstr("CS_GNU_LIBC_VERSION") raises ValueError.
        with mock.patch.object(
            os, "confstr", side_effect=ValueError("unrecognized configuration name")
        ):
            self.assertEqual(_gnu_libc_version(), "")

    def test_no_confstr_means_no_glibc(self) -> None:
        # Windows: the os module has no confstr.
        with mock.patch.object(os, "confstr", side_effect=AttributeError):
            self.assertEqual(_gnu_libc_version(), "")

    def test_glibc_value_is_passed_through(self) -> None:
        with mock.patch.object(os, "confstr", return_value="glibc 2.44"):
            self.assertEqual(_gnu_libc_version(), "glibc 2.44")

    def test_trace_test_skips_without_glibc(self) -> None:
        # The skip names its precondition and the trace is not attempted.
        case = LoaderTraceTest("test_trace_does_not_run_the_program")
        with (
            mock.patch.object(os, "confstr", side_effect=ValueError),
            mock.patch(f"{__name__}._loader_trace", side_effect=AssertionError("traced")),
        ):
            with self.assertRaises(unittest.SkipTest) as raised:
                case.test_trace_does_not_run_the_program()
        self.assertIn("needs glibc's dynamic loader", str(raised.exception))


class LoaderTraceTest(unittest.TestCase):
    """The trace runs nothing, so no program or IFUNC resolver can crash it."""

    def test_trace_does_not_run_the_program(self) -> None:
        libc = _gnu_libc_version()
        if not libc.startswith("glibc"):
            self.skipTest(f"needs glibc's dynamic loader (C library: {libc or 'not glibc'})")
        false = shutil.which("false")
        if false is None or "(NEEDED)" not in _readelf("-d", false):
            self.skipTest("no dynamically linked `false` to trace")
        # Run, `false` exits 1 and the old trace raised; traced, it binds
        # its references to libc and exits 0.
        trace = _loader_trace(Path(false))
        targets = {Path(m.group(2)).name for m in BINDING.finditer(trace)}
        self.assertTrue(any(name.startswith("libc.so") for name in targets), trace[-2000:])


class ThisBuildTest(unittest.TestCase):
    """The binaries of the build this test runs in (core/test/meson.build)."""

    def setUp(self) -> None:
        root = os.environ.get(BUILD_ROOT_ENV)
        if not root:
            self.skipTest(f"{BUILD_ROOT_ENV} is not set: no build to read")
        c_id, cpp_id = _build_compiler_ids(Path(root))
        if INTEL_LLVM not in (c_id, cpp_id):
            self.skipTest(
                f"not an icx / icpx build (C: {c_id}, C++: {cpp_id}); nothing links libimf"
            )
        self.library = os.environ.get(LIBRARY_ENV, "")
        self.cli = os.environ.get(CLI_ENV, "")

    def test_library_imports_the_math_functions_from_glibc(self) -> None:
        if not self.library:
            self.skipTest("a static-only build has no shared libvmaf; the CLI checks cover it")
        failures = library_failures(
            _readelf("-d", self.library), _readelf("-W", "--dyn-syms", self.library)
        )
        self.assertEqual(failures, [])

    def test_cli_does_not_export_the_math_functions(self) -> None:
        if not self.cli:
            self.skipTest("built with -Denable_tools=false: no CLI")
        self.assertEqual(
            cli_failures(_readelf("-d", self.cli), _readelf("-W", "--dyn-syms", self.cli)), []
        )

    def test_loader_binds_every_reference_to_libm(self) -> None:
        if not self.cli:
            self.skipTest("built with -Denable_tools=false: no CLI")
        self.assertEqual(binding_failures(_loader_trace(Path(self.cli)), ("libvmaf", "vmaf")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
