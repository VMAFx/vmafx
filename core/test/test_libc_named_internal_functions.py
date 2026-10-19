#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""No internal C function carries the name of a C library or POSIX function.

macOS's headers declare many C library functions with an assembler label:
`<unistd.h>` declares `int close(int) __DARWIN_ALIAS_C(close)`, which is
`__asm("_close")` on arm64 and x86-64. In a full-LTO link every module is
merged into one, and the label keeps the declaration's IR name (`\\01_close`)
apart from a `static int close(...)` of another file (IR name `close`), so the
linker does not rename the static one; both then become the assembler symbol
`_close`, and every call of the C library's `close()` in the merged module
branches to the static function. The release builds of macOS (`b_lto=true`)
did exactly that: `test_adm_coverage` crashed in an extractor's
`close(VmafFeatureExtractor *)` called with a file descriptor
(T-DARWIN-LTO-STATIC-CLOSE-COLLISION-2026-10-06), and the `close()` calls on
the fdopen() failure paths of `libvmaf.c`, `cambi.c` and `svm.cpp` were the
same call. glibc declares these functions without a label, so Linux builds
never showed it.

The test reads the C sources and headers under core/ and fails on a function
with internal linkage (`static`) named after one of the functions below. C++
translation units are not scanned: their internal functions have mangled
names. The list holds the C library and POSIX functions of the headers macOS
labels (`<unistd.h>`, `<fcntl.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`,
`<sys/stat.h>`, `<sys/select.h>`, `<sys/socket.h>`, `<sys/wait.h>`,
`<poll.h>`, `<signal.h>`, `<time.h>`, `<pthread.h>`) and the I/O functions
next to them; a name is listed whether or not macOS labels it, so the rule
does not depend on one SDK release.

Device-free and compiler-free: reads the sources only.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
SCANNED = ("src", "test", "tools")
SUFFIXES = {".c", ".h"}
# The Windows shims define the POSIX functions MSVC lacks, as static inline
# functions of those names on purpose; they are compiled on Windows only.
SHIMS = ("src/compat/win32/", "tools/compat/win32/")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# `static [inline] <type> name(` with the type on the same or the previous line.
INTERNAL = re.compile(r"^[ \t]*static\b[^;{}()=]*?\b([A-Za-z_]\w*)\s*\(", re.M)

LIBC_NAMES = frozenset(
    [
        "access",
        "accept",
        "alarm",
        "bind",
        "chdir",
        "chmod",
        "chown",
        "close",
        "closedir",
        "confstr",
        "connect",
        "creat",
        "dup",
        "dup2",
        "execv",
        "execve",
        "execvp",
        "exit",
        "fchmod",
        "fchown",
        "fclose",
        "fcntl",
        "fdopen",
        "fflush",
        "fgets",
        "fileno",
        "fopen",
        "fork",
        "fprintf",
        "fputs",
        "fread",
        "freopen",
        "fscanf",
        "fseek",
        "fstat",
        "fsync",
        "ftell",
        "ftruncate",
        "fwrite",
        "getcwd",
        "getline",
        "getopt",
        "getpeername",
        "getsockname",
        "getsockopt",
        "kill",
        "lchown",
        "link",
        "listen",
        "lockf",
        "lseek",
        "lstat",
        "malloc",
        "free",
        "calloc",
        "realloc",
        "mkdir",
        "mkfifo",
        "mkstemp",
        "mktime",
        "mmap",
        "mprotect",
        "msync",
        "munmap",
        "nanosleep",
        "nice",
        "open",
        "openat",
        "opendir",
        "pause",
        "pclose",
        "pipe",
        "poll",
        "popen",
        "pread",
        "printf",
        "pselect",
        "putenv",
        "pwrite",
        "read",
        "readdir",
        "readlink",
        "realpath",
        "recv",
        "recvfrom",
        "recvmsg",
        "remove",
        "rename",
        "rewind",
        "rmdir",
        "scanf",
        "select",
        "send",
        "sendmsg",
        "sendto",
        "setenv",
        "setsockopt",
        "setvbuf",
        "shutdown",
        "sigaction",
        "sigaltstack",
        "signal",
        "sigsuspend",
        "sigwait",
        "sleep",
        "snprintf",
        "socket",
        "socketpair",
        "sprintf",
        "sscanf",
        "stat",
        "strerror",
        "strftime",
        "strtod",
        "symlink",
        "system",
        "tmpfile",
        "truncate",
        "unlink",
        "unsetenv",
        "usleep",
        "vfprintf",
        "vsnprintf",
        "wait",
        "waitid",
        "waitpid",
        "write",
        "pthread_cond_signal",
        "pthread_cond_timedwait",
        "pthread_cond_wait",
        "pthread_create",
        "pthread_join",
        "pthread_mutex_lock",
        "pthread_mutex_unlock",
        "pthread_once",
        "pthread_testcancel",
    ]
)


def libc_named_internal_functions(root: Path) -> list[str]:
    """`file: name` for every internal function named after a LIBC_NAMES entry."""
    found = []
    for sub in SCANNED:
        for path in sorted((root / sub).rglob("*")):
            rel = path.relative_to(root).as_posix()
            if path.suffix not in SUFFIXES or rel.startswith(SHIMS):
                continue
            code = COMMENT.sub("", path.read_text(encoding="utf-8", errors="replace"))
            for name in INTERNAL.findall(code):
                if name in LIBC_NAMES:
                    found.append(f"{rel}: {name}")
    return found


class LibcNamedInternalFunctions(unittest.TestCase):
    def test_no_internal_function_has_a_libc_name(self) -> None:
        self.assertEqual(libc_named_internal_functions(CORE), [])

    def test_the_scan_sees_a_static_close(self) -> None:
        # The planted form of the defect: the extractor close() of master.
        code = "static int\nclose(VmafFeatureExtractor *fex)\n{\n    return 0;\n}\n"
        self.assertEqual(INTERNAL.findall(code), ["close"])
        inline = "static inline FILE *fopen(const char *p, const char *m);"
        self.assertEqual(INTERNAL.findall(inline), ["fopen"])

    def test_the_scan_ignores_calls_and_other_names(self) -> None:
        code = "static int close_fex(VmafFeatureExtractor *fex)\n{\n    (void)close(fd);\n}\n"
        self.assertEqual([n for n in INTERNAL.findall(code) if n in LIBC_NAMES], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
