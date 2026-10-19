#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Download the Go modules the Meson suite needs, before the suite runs.

Four tests of the suite run Go tooling (GO_TOOL_TESTS): `go tool
controller-gen`, the pinned buf with `go tool protoc-gen-go*`, and `go test
./cmd/vmafx-server/`. On a cold module cache each of them downloaded what it
needed from the module proxy inside the test, once: one dropped stream failed a
required job (T-CI-GO-MODULES-DOWNLOADED-IN-TESTS-2026-10-10).

A job that runs the suite calls this script first, as the user and with the
environment the suite runs in (the module cache and the tool cache belong to a
user)::

    python3 scripts/ci/prefetch_go_modules.py

It runs `go mod download` (every module go.mod requires, the tools' included,
and the Go toolchain go.mod names when the installed one is older) and `go
install` of the pinned buf into the tool cache, where
scripts/codegen/proto_generate.py finds it. Each runs up to ATTEMPTS times with
the waits of RETRY_DELAYS_S in between; a retry keeps what the attempts before
it stored. With both done the four tests need no network: `go run
<package>@<version>`, which proto_generate.py falls back to, asks the proxy for
the module's versions on every run.

Exit 0: the modules and buf are in place, or Go is not on PATH (the four tests
then skip themselves with exit 77; the reason is printed). Exit 1: a download
failed on every attempt; the message names it and carries the last error.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import sys
import time
from collections.abc import Callable
from pathlib import Path
from types import ModuleType
from typing import NamedTuple

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandOutputLimitExceeded,
    CommandTimedOut,
)
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

NAME = "prefetch_go_modules"
# The Meson tests that launch Go; scripts/ci/tests/test_meson_suite_go_prefetch.py
# holds this list against the scripts those tests run.
GO_TOOL_TESTS = (
    "test_crd_generated_current",
    "test_crd_compat",
    "test_proto_generated_current",
    "test_vmafx_score_contract",
)
# Waits between attempts: five attempts, 220 s of waiting. The failure this
# answers was one dropped HTTP/2 stream; the waits also outlast a proxy that
# answers 5xx for a few minutes.
RETRY_DELAYS_S: tuple[float, ...] = (10, 30, 60, 120)
ATTEMPTS = len(RETRY_DELAYS_S) + 1
# One attempt: the cold downloads took 100 to 145 s inside the tests on a hosted
# runner (run 38069385197), the build of buf included.
ATTEMPT_TIMEOUT_S = 600.0
ERROR_TAIL_CHARS = 2000
PROTO_GENERATE = Path("scripts/codegen/proto_generate.py")


class Download(NamedTuple):
    """One command that fills the module cache or the tool cache."""

    what: str
    argv: tuple[str, ...]
    environment: dict[str, str]  # added to this process's environment


def proto_generate() -> ModuleType:
    """scripts/codegen/proto_generate.py, which owns the buf pin and where its
    installed copy lives. Loaded by path: scripts/codegen is a directory of
    scripts, not a package."""
    spec = importlib.util.spec_from_file_location("proto_generate", REPO / PROTO_GENERATE)
    if spec is None or spec.loader is None:
        raise SystemExit(f"{NAME}: cannot load {PROTO_GENERATE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def buf_download(go: str, root: Path, proto: ModuleType) -> Download:
    """`go install` of the pinned buf into the tool cache."""
    argv, environment = proto.buf_install(root)
    return Download(f"buf ({argv[-1]}, go install)", (go, *argv[1:]), dict(environment))


def attempt(download: Download, root: Path) -> str | None:
    """Run one download once; None when it succeeded, else why it did not."""
    try:
        out = run_command(
            download.argv,
            allowed_executables=(download.argv[0],),
            cwd=root,
            env={**os.environ, **download.environment},
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            timeout_seconds=ATTEMPT_TIMEOUT_S,
        )
    except CommandTimedOut:
        return f"ran over {ATTEMPT_TIMEOUT_S:g} s"
    except CommandOutputLimitExceeded:
        return "printed more than the output limit"
    if out.returncode == 0:
        return None
    text = (out.stderr.strip() or out.stdout.strip())[-ERROR_TAIL_CHARS:]
    return text or f"exit status {out.returncode}"


def fetch(
    download: Download,
    root: Path,
    delays: tuple[float, ...],
    sleep: Callable[[float], None],
) -> str | None:
    """Run one download until it succeeds; None, or the last error after
    len(delays) + 1 failed attempts."""
    error: str | None = None
    for number, delay in enumerate((*delays, None), start=1):
        error = attempt(download, root)
        if error is None:
            print(f"{NAME}: {download.what}: done (attempt {number})", flush=True)
            return None
        if delay is None:
            break
        print(
            f"{NAME}: {download.what}: attempt {number} of {len(delays) + 1} failed, "
            f"next in {delay:g} s:\n{error}",
            flush=True,
        )
        sleep(delay)
    return error


def annotate(level: str, message: str) -> None:
    """Print a message; on GitHub Actions also as a one-line annotation."""
    stream = sys.stderr if level == "error" else sys.stdout
    print(f"{NAME}: {message}", file=stream, flush=True)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{level}::{NAME}: {message.splitlines()[0]}", flush=True)


def failed(download: Download, delays: tuple[float, ...], error: str) -> int:
    annotate(
        "error",
        f"could not download {download.what} in {len(delays) + 1} attempts "
        f"over {sum(delays):g} s of waiting; the tests that run Go tooling "
        f"({', '.join(GO_TOOL_TESTS)}) need it. Last error:\n{error}",
    )
    return 1


def prefetch(
    root: Path,
    delays: tuple[float, ...] = RETRY_DELAYS_S,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """Fill the caches; 0 when every download succeeded or Go is absent."""
    go = shutil.which("go")
    if go is None:
        annotate(
            "notice",
            "SKIP: go is not on PATH, nothing to download; "
            f"{', '.join(GO_TOOL_TESTS)} skip themselves (exit 77)",
        )
        return 0
    modules = Download("the modules go.mod requires (go mod download)", (go, "mod", "download"), {})
    error = fetch(modules, root, delays, sleep)
    if error is not None:
        return failed(modules, delays, error)
    proto = proto_generate()
    if proto.buf_installed(root):
        print(f"{NAME}: buf: the pinned release is in the tool cache already", flush=True)
        return 0
    buf = buf_download(go, root, proto)
    error = fetch(buf, root, delays, sleep)
    if error is not None:
        return failed(buf, delays, error)
    if not proto.buf_installed(root):
        annotate(
            "error",
            f"{' '.join(buf.argv[1:])} succeeded, but {buf.environment['GOBIN']} holds no "
            "buf that reports the pinned version",
        )
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO, help="repository root")
    args = parser.parse_args(argv)
    return prefetch(args.root.resolve())


if __name__ == "__main__":
    sys.exit(main())
