#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every pthread function the C tree calls exists in the Windows shim.

MSVC builds get their threads from `core/src/compat/win32/pthread.h`, a small
shim over Win32 primitives. A call to a pthread function the shim does not
define compiles on every POSIX host and fails to link on Windows only
(`LNK2001`), which is how `test_async_extractor_thread_pool` kept the Windows
ARM64 MSVC build red from 2026-10-01 (`pthread_self`, `pthread_equal`).

A function may be used without being in the shim only when the build probes
for it and leaves the caller out where it is missing; those are listed in
`PROBED` with the Meson variable that guards them, and the test checks that the
guard is still in `core/test/meson.build`.

The shim has a timed wait (Q-302): `pthread_cond_timedwait()` converts its
CLOCK_REALTIME deadline with `vmaf_w32_timeout_ms()` and reports ETIMEDOUT only
once the clock has passed it, and the host fences of `core/src/vmafx/fence.c`
wait on it instead of polling.

Device-free and compiler-free: reads the sources only.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
SHIM = CORE / "src" / "compat" / "win32" / "pthread.h"
SCANNED = ("src", "test", "tools")
SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
CALL = re.compile(r"\b(pthread_[a-z_]+)\s*\(")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

# function -> (file that may call it, Meson guard that keeps the file out of a
# build without the function). Empty since the shim has a timed wait; the
# mechanism stays for the next function a platform lacks.
PROBED: dict[str, tuple[str, str]] = {}
FENCE = CORE / "src" / "vmafx" / "fence.c"

# function -> (file, preprocessor names). The calls compile only where one of
# the names is true; the test checks that every call sits in the true branch of
# an #if / #ifdef naming one of them. fence.c selects CLOCK_MONOTONIC for its
# condition variables on Linux (VMAFX_HOST_FENCE_COND_CLOCK, defined under
# `#if defined(__linux__)`).
PLATFORM_ONLY = dict.fromkeys(
    ("pthread_condattr_init", "pthread_condattr_setclock", "pthread_condattr_destroy"),
    ("src/vmafx/fence.c", ("VMAFX_HOST_FENCE_COND_CLOCK", "__linux__")),
)
DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def calls_in_true_branch(text: str, function: str, names: tuple[str, ...]) -> bool:
    """True when every call of `function` in `text` sits in the true branch of
    an #if / #ifdef whose condition names one of `names`."""
    stack: list[tuple[str, bool]] = []  # (directive + condition, in #else)
    for line in COMMENT.sub("", text).splitlines():
        match = DIRECTIVE.match(line)
        if match:
            kind, rest = match.group(1), match.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                stack.append((kind + rest, False))
            elif kind == "elif" and stack:
                stack[-1] = ("if" + rest, False)
            elif kind == "else" and stack:
                stack[-1] = (stack[-1][0], True)
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if re.search(rf"\b{function}\s*\(", line):
            guarded = any(
                not in_else and not cond.startswith("ifndef") and any(n in cond for n in names)
                for cond, in_else in stack
            )
            if not guarded:
                return False
    return True


def shim_functions(text: str) -> set[str]:
    return set(CALL.findall(COMMENT.sub("", text)))


def calls_outside_shim(root: Path) -> dict[str, set[str]]:
    """Map each called pthread function to the files (relative to core/) calling it."""
    found: dict[str, set[str]] = {}
    for sub in SCANNED:
        for path in sorted((root / sub).rglob("*")):
            if path.suffix not in SUFFIXES or "compat/win32" in path.as_posix():
                continue
            code = COMMENT.sub("", path.read_text(encoding="utf-8", errors="replace"))
            for name in CALL.findall(code):
                found.setdefault(name, set()).add(path.relative_to(root).as_posix())
    return found


def missing_from_shim(root: Path) -> list[str]:
    provided = shim_functions(SHIM.read_text(encoding="utf-8"))
    missing = []
    for name, files in sorted(calls_outside_shim(root).items()):
        if name in provided:
            continue
        allowed = PROBED.get(name, ("", ""))[0]
        guarded_file, names = PLATFORM_ONLY.get(name, ("", ()))
        for file in sorted(files):
            if file == allowed:
                continue
            if file == guarded_file and calls_in_true_branch(
                (root / file).read_text(encoding="utf-8"), name, names
            ):
                continue
            missing.append(f"{file}: {name}")
    return missing


class Win32PthreadShimContract(unittest.TestCase):
    def test_every_called_pthread_function_is_in_the_shim(self) -> None:
        self.assertEqual(missing_from_shim(CORE), [])

    def test_probed_functions_keep_their_meson_guard(self) -> None:
        meson = (CORE / "test" / "meson.build").read_text(encoding="utf-8")
        for name, (file, guard) in PROBED.items():
            target = Path(file).stem
            block = re.search(rf"if {guard}\n\s+{target} = executable\(", meson)
            self.assertIsNotNone(block, f"{name}: {target} is no longer built under 'if {guard}'")

    def test_a_call_in_a_comment_is_not_a_call(self) -> None:
        self.assertEqual(shim_functions("/* pthread_self() */ // pthread_equal(a, b)\n"), set())

    def test_the_shim_has_a_timed_wait(self) -> None:
        shim = COMMENT.sub("", SHIM.read_text(encoding="utf-8"))
        self.assertIn("pthread_cond_timedwait", shim_functions(shim))
        body = shim[shim.index("int pthread_cond_timedwait(") :]
        body = body[: body.index("\n}\n")]
        # The deadline is converted before the wait and checked again after a
        # Win32 timeout: ETIMEDOUT only once the clock has passed it.
        self.assertEqual(body.count("vmaf_w32_timeout_ms("), 2)
        self.assertIn("ERROR_TIMEOUT", body)
        self.assertIn("ETIMEDOUT", body)
        self.assertIn("SleepConditionVariableSRW(", body)

    def test_host_fences_wait_on_the_condition(self) -> None:
        fence = COMMENT.sub("", FENCE.read_text(encoding="utf-8"))
        self.assertIn("pthread_cond_timedwait(&fence->cond, &fence->lock,", fence)
        self.assertIn("pthread_cond_broadcast(&fence->cond)", fence)

    def test_a_platform_call_outside_its_guard_is_reported(self) -> None:
        guarded = "#if defined(__linux__)\nvoid f(void) { pthread_condattr_init(&a); }\n#endif\n"
        in_else = "#ifdef VMAFX_HOST_FENCE_COND_CLOCK\n#else\nvoid f(void) { pthread_condattr_init(&a); }\n#endif\n"
        bare = "void f(void) { pthread_condattr_init(&a); }\n"
        names = ("VMAFX_HOST_FENCE_COND_CLOCK", "__linux__")
        self.assertTrue(calls_in_true_branch(guarded, "pthread_condattr_init", names))
        self.assertFalse(calls_in_true_branch(in_else, "pthread_condattr_init", names))
        self.assertFalse(calls_in_true_branch(bare, "pthread_condattr_init", names))

    def test_an_unshimmed_call_is_reported(self) -> None:
        code = "static void f(void) { (void)pthread_self(); }\n"
        self.assertEqual(set(CALL.findall(COMMENT.sub("", code))), {"pthread_self"})
        self.assertNotIn("pthread_self", shim_functions(SHIM.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
