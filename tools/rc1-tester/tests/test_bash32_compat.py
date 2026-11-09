# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Everything that runs on macOS must work in bash 3.2 (Apple's) or POSIX sh.

The hosted macOS runner's `bash` is 3.2: a `mapfile` in the bundle build script failed
there with exit 127. Two checks: a static scan for bash 4+ constructs, and, when Docker
and the `bash:3.2` image are available, the real scripts run under bash 3.2 with stub
tools (the stub run is skipped, not failed, without Docker).
"""

from __future__ import annotations

import contextlib
import os
import re
import shutil
import stat
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

SCRIPT_TIMEOUT_S = 60
SUBPROCESS_TIMEOUT_S = 60

BASH = shutil.which("bash")
DOCKER = shutil.which("docker")
SHELLCHECK = shutil.which("shellcheck")

_ROOT = Path(__file__).resolve().parents[3]
BUILD_SH = _ROOT / "scripts" / "ci" / "build-macos-tester-bundle.sh"
LINKS_SH = _ROOT / "scripts" / "ci" / "check-macos-bundle-links.sh"
RUN_SH = _ROOT / "tools" / "rc1-tester" / "image" / "macos" / "run.sh"
WORKFLOW = _ROOT / ".github" / "workflows" / "macos-tester-bundle.yml"

BASH4 = {
    "mapfile/readarray": re.compile(r"\b(mapfile|readarray)\b"),
    "declare -A": re.compile(r"\b(declare|typeset|local)\s+-[a-zA-Z]*A"),
    "case conversion": re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*(,,|\^\^|,|\^)\}"),
    "|&": re.compile(r"\|&"),
    ";& / ;;&": re.compile(r";;&|;&"),
    "[[ -v ]]": re.compile(r"\[\[\s+-v\s"),
    "coproc": re.compile(r"\bcoproc\b"),
    "local -n": re.compile(r"\b(local|declare)\s+-[a-zA-Z]*n\b"),
    "&>>": re.compile(r"&>>"),
}


def code_lines(text: str) -> list[tuple[int, str]]:
    return [
        (n, line)
        for n, line in enumerate(text.splitlines(), 1)
        if not line.lstrip().startswith("#")
    ]


def findings(text: str) -> list[str]:
    return [
        f"line {n}: {name}"
        for n, line in code_lines(text)
        for name, pattern in BASH4.items()
        if pattern.search(line)
    ]


def macos_build_job_steps() -> str:
    text = WORKFLOW.read_text(encoding="utf-8")
    return text[text.index("\n  build:") : text.index("\n  publish:")]


@pytest.mark.parametrize("path", [BUILD_SH, LINKS_SH, RUN_SH], ids=lambda p: p.name)
def test_macos_scripts_have_no_bash4_constructs(path: Path) -> None:
    assert findings(path.read_text(encoding="utf-8")) == []


def test_macos_build_job_steps_have_no_bash4_constructs() -> None:
    assert findings(macos_build_job_steps()) == []


def test_the_scan_catches_what_broke_the_runner() -> None:
    assert findings("mapfile -t x < <(ls)") == ["line 1: mapfile/readarray"]
    assert findings("declare -A m") and findings('echo "${x,,}"') and findings("a |& b")
    assert findings("[[ -v name ]]") and findings("local -n ref=x") and findings("coproc x { :; }")
    assert findings("# mapfile is not allowed\necho ok") == []  # comments are not code
    assert findings('arr+=("$x")\nwhile IFS= read -r l; do :; done') == []  # the replacement


def test_run_sh_is_posix_sh() -> None:
    """Assert run.sh starts with a POSIX sh shebang and passes shellcheck."""
    assert RUN_SH.read_text(encoding="utf-8").startswith("#!/bin/sh\n")
    if SHELLCHECK is None:
        pytest.skip("shellcheck not installed")
    result = subprocess.run(
        [SHELLCHECK, "--shell=sh", str(RUN_SH)],
        capture_output=True,
        text=True,
        check=False,
        timeout=SCRIPT_TIMEOUT_S,
    )
    assert result.returncode == 0, result.stdout


# ---- run the real scripts under bash 3.2 -------------------------------------------------


def bash32_available() -> bool:
    """Return True if docker is installed and the bash:3.2 image is available."""
    if DOCKER is None:
        return False
    try:
        probe = subprocess.run(
            [DOCKER, "image", "inspect", "bash:3.2"],
            capture_output=True,
            check=False,
            timeout=SUBPROCESS_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        return False
    return probe.returncode == 0


# CI pulls the image and sets VMAFX_REQUIRE_BASH32=1: a skip there would hide a regression of the
# build script (zimg became a dependency and the stubbed run broke while CI skipped it).
REQUIRE_BASH32 = os.environ.get("VMAFX_REQUIRE_BASH32") == "1"


def bash32_skip_reason(available: bool, required: bool) -> str | None:
    """Why the docker tests skip, None when they run; a required image that is missing fails."""
    if available:
        return None
    if required:
        pytest.fail("VMAFX_REQUIRE_BASH32=1 but docker image bash:3.2 is not available")
    return "docker image bash:3.2 not available (docker pull bash:3.2)"


def test_bash32_available_returns_false_on_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    """Assert bash32_available returns False when the docker probe raises TimeoutExpired."""

    def fake_run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        raise subprocess.TimeoutExpired(cmd=["docker"], timeout=SUBPROCESS_TIMEOUT_S)

    monkeypatch.setattr(subprocess, "run", fake_run)
    monkeypatch.setattr(sys.modules[__name__], "DOCKER", "/usr/bin/docker")
    assert bash32_available() is False


def test_bash32_available_returns_false_when_docker_none(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Assert bash32_available returns False when docker executable is not found."""
    monkeypatch.setattr(sys.modules[__name__], "DOCKER", None)
    assert bash32_available() is False


def test_required_bash32_never_skips() -> None:
    """Negative: with the image required, a missing image fails instead of skipping."""
    assert bash32_skip_reason(True, True) is None
    assert bash32_skip_reason(False, False) is not None
    with pytest.raises(pytest.fail.Exception):
        bash32_skip_reason(False, True)


_BASH32_SKIP = bash32_skip_reason(bash32_available(), REQUIRE_BASH32)
needs_bash32 = pytest.mark.skipif(_BASH32_SKIP is not None, reason=_BASH32_SKIP or "")


def stub(path: Path, body: str) -> None:
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


# `shasum -c -` reads its list from a pipe (`echo "$SHA  $file" | shasum -a 256 -c -` in the
# build script). A stub that exits without reading lets the writer meet a closed pipe when it
# runs late: SIGPIPE, the pipeline exits 141 under pipefail and `set -e` ends the script with
# nothing on stderr. That was the flake of test_build_script_runs_to_the_end_under_bash32 on
# a loaded host (stdout stopped at the licence-texts step, stderr empty). The stub drains its
# input as the real tool does.
SHASUM_STUB = 'case "$*" in *" -") cat >/dev/null ;; esac\nexit 0\n'


def test_shasum_stub_reads_its_input_like_shasum(tmp_path: Path) -> None:
    """A writer that runs after the stub started still finds an open pipe."""
    stub(tmp_path / "shasum", SHASUM_STUB)
    late_writer = (
        f"set -euo pipefail; (sleep 0.3; echo \"0123  /w/x\") | {tmp_path / 'shasum'} -a 256 -c -; "
        "echo reached"
    )
    result = subprocess.run(
        [BASH, "-c", late_writer],
        capture_output=True,
        text=True,
        check=False,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    assert result.returncode == 0 and "reached" in result.stdout, (result.returncode, result.stderr)


def _remove_container(name: str) -> None:
    """Remove a docker container by name, ignoring timeout if removal hangs."""
    with contextlib.suppress(subprocess.TimeoutExpired):
        subprocess.run(
            [DOCKER, "rm", "--force", name],
            capture_output=True,
            check=False,
            timeout=SUBPROCESS_TIMEOUT_S,
        )


def docker_bash32(mount: Path, command: str) -> subprocess.CompletedProcess:
    """Execute a command in a bash:3.2 docker container and return the process result."""
    name = f"vmafx-bash32-{uuid.uuid4().hex[:12]}"
    try:
        return subprocess.run(
            [
                DOCKER,
                "run",
                "--rm",
                "--name",
                name,
                "--network",
                "none",
                "--user",
                f"{os.getuid()}:{os.getgid()}",
                "-v",
                f"{mount}:/w",
                "-v",
                f"{_ROOT}:/repo:ro",
                "-w",
                "/repo",
                "bash:3.2",
                "bash",
                "-c",
                command,
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=SUBPROCESS_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        _remove_container(name)
        pytest.fail(
            f"docker_bash32 timed out after {SUBPROCESS_TIMEOUT_S}s running {command!r} "
            f"(container {name})"
        )


def test_docker_bash32_raises_pytest_fail_on_timeout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Assert docker_bash32 fails naming the bound in seconds and the command string."""
    cmd = "echo slow_command_test"

    def fake_run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        raise subprocess.TimeoutExpired(cmd=["docker"], timeout=SUBPROCESS_TIMEOUT_S)

    monkeypatch.setattr(subprocess, "run", fake_run)
    pattern = rf"{SUBPROCESS_TIMEOUT_S}.*{re.escape(cmd)}"
    with pytest.raises(pytest.fail.Exception, match=pattern):
        docker_bash32(tmp_path, cmd)


def test_docker_bash32_removes_the_container_on_timeout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Assert docker_bash32 removes the container on timeout and names it in the failure."""
    calls: list[list[str]] = []

    def fake_run(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(argv))
        if len(argv) > 1 and argv[1] == "run":
            raise subprocess.TimeoutExpired(cmd=argv, timeout=SUBPROCESS_TIMEOUT_S)
        return subprocess.CompletedProcess(args=argv, returncode=0, stdout="", stderr="")

    monkeypatch.setattr(subprocess, "run", fake_run)
    monkeypatch.setattr(sys.modules[__name__], "DOCKER", "/usr/bin/docker")

    with pytest.raises(pytest.fail.Exception) as exc_info:
        docker_bash32(tmp_path, "echo test")

    assert len(calls) == 2
    first_argv, second_argv = calls[0], calls[1]
    name_idx = first_argv.index("--name")
    name = first_argv[name_idx + 1]
    assert name.startswith("vmafx-bash32-")
    assert second_argv == ["/usr/bin/docker", "rm", "--force", name]
    assert name in str(exc_info.value)


@needs_bash32
def test_links_check_runs_under_bash32(tmp_path: Path) -> None:
    fake = tmp_path / "fake"
    fake.mkdir()
    stub(fake / "file", 'case "$2" in *.macho) echo "Mach-O 64-bit arm64";; *) echo text;; esac\n')
    stub(fake / "otool", 'echo "$2:"; printf "\\t/usr/lib/libSystem.B.dylib (compat)\\n"\n')
    bundle = tmp_path / "bundle" / "build"
    bundle.mkdir(parents=True)
    (bundle / "vmaf.macho").write_text("x")
    cmd = "PATH=/w/fake:$PATH bash scripts/ci/check-macos-bundle-links.sh /w/bundle"
    result = docker_bash32(tmp_path, cmd)
    assert result.returncode == 0, result.stderr


@needs_bash32
def test_build_script_runs_to_the_end_under_bash32(tmp_path: Path) -> None:
    """The whole build script under bash 3.2 with stub tools: it must reach `pack`."""
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    for name in ("meson", "codesign", "zstd"):
        stub(stubs / name, "exit 0\n")
    # The script links zimg statically from `brew --prefix zimg`: a prefix with libzimg.a and
    # the zimg.pc whose `-L${libdir} -lzimg` it rewrites to that archive.
    zimg = tmp_path / "zimg"
    (zimg / "lib" / "pkgconfig").mkdir(parents=True)
    (zimg / "lib" / "libzimg.a").write_text("")
    (zimg / "lib" / "pkgconfig" / "zimg.pc").write_text("Libs: -L${libdir} -lzimg\n")
    stub(stubs / "brew", 'case "$1 $2" in "--prefix zimg") echo /w/zimg;; esac\n')
    stub(stubs / "shasum", SHASUM_STUB)
    stub(
        stubs / "ninja",
        "mkdir -p /tmp/repo-copy/build-tester-macos/tools; : > /tmp/repo-copy/build-tester-macos/tools/vmaf\n",
    )
    stub(stubs / "git", 'case "$1" in rev-parse) echo /tmp/repo-copy;; esac\n')
    stub(
        stubs / "curl",
        'while [ $# -gt 1 ]; do [ "$1" = -o ] && { mkdir -p "$(dirname "$2")"; : > "$2"; }; shift; done\n',
    )
    stub(stubs / "stat", 'echo "1 $*"\n')
    stub(
        stubs / "tar",
        'case "$1" in -xzf) mkdir -p "$4/python/bin" "$4/python/include" "$4/python/share" "$4/python/lib/python3.13/lib-dynload"; printf "#!/bin/sh\\nexit 0\\n" > "$4/python/bin/python3"; chmod +x "$4/python/bin/python3";; '
        '-xf) mkdir -p "$4/python/licenses"; : > "$4/python/licenses/LICENSE.cpython.txt"; : > "$4/python/PYTHON.json";; '
        '--uid) while [ $# -gt 1 ]; do [ "$1" = -cJf ] && : > "$2"; shift; done;; *) : ;; esac\n',
    )
    stub(stubs / "otool", 'echo "$2:"; printf "\\t/usr/lib/libSystem.B.dylib (c)\\n"\n')
    stub(stubs / "file", "echo text\n")
    stub(stubs / "uname", 'case "$1" in -s) echo Darwin;; -m) echo arm64;; esac\n')
    stub(
        stubs / "python3",
        'case "$2" in select) echo test/t1; echo test/t2;; stage) mkdir -p "$5/tests"; : > "$5/tests/t1";; esac\n',
    )
    # The repo is mounted read-only; the script cds into `git rev-parse --show-toplevel`.
    repo_copy = tmp_path / "repo-copy"
    shutil.copytree(_ROOT / "scripts" / "ci", repo_copy / "scripts" / "ci")
    shutil.copytree(_ROOT / "tools" / "rc1-tester", repo_copy / "tools" / "rc1-tester",
                    ignore=shutil.ignore_patterns("tests", "__pycache__"))  # fmt: skip
    env = ("PBS_URL=x PBS_SHA256=y PBS_FULL_URL=u PBS_FULL_SHA256=v VMAF_RESOURCE_COMMIT=z VMAFX_SOURCE_COMMIT=a "
           "VMAFX_SOURCE_REF=b VMAFX_RECIPE_COMMIT=c VMAFX_IMAGE_TAG=t")  # fmt: skip
    cmd = (f"cp -R /w/repo-copy /tmp/repo-copy && cd /tmp/repo-copy && "
           f"{env} PATH=/w/stubs:$PATH bash scripts/ci/build-macos-tester-bundle.sh /w/out; echo status=$?")  # fmt: skip
    result = docker_bash32(tmp_path, cmd)
    assert "status=0" in result.stdout, result.stderr
    assert "mapfile" not in result.stderr, result.stderr
    assert "command not found" not in result.stderr.replace("sandbox-exec", ""), result.stderr
    # The interpreter archive is an input: nothing but the bundle's own files may be
    # left in the output directory, whose archives the workflow publishes.
    assert (tmp_path / "out" / "vmafx-tester-macos-arm64-t.tar.xz").exists()
    assert not (tmp_path / "out" / "pbs.tar.gz").exists()
    assert not (tmp_path / "out" / "pbs").exists()
    assert not (tmp_path / "out" / "pbs-full.tar.zst").exists()
    assert not (tmp_path / "out" / "python").exists()
