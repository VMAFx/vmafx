# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every GPU runtime stage of the tester image finds the libraries its build stage stages.

The GPU build stages of `docker/Dockerfile.tester` copy the shared libraries the
`vmaf` binary links (libzimg since zimg became a default dependency) into
`/opt/vmafx/lib`. The matching runtime stage must copy that directory and put it
on `LD_LIBRARY_PATH`, or the stage's `ldd` check stops the image build with
`libzimg.so.2 => not found`. The GPU images build only on a push to master, so
this test is the pull-request coverage of the contract.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
DOCKERFILE = REPO / "docker/Dockerfile.tester"
KITS = ("sycl", "cuda", "hip")


def _stage(text: str, name: str) -> str:
    """The body of stage `name`, up to the next FROM line."""
    match = re.search(rf"^FROM \S+ AS {re.escape(name)}\n", text, re.MULTILINE)
    assert match, f"stage {name} not found"
    body = text[match.end() :]
    end = re.search(r"^FROM ", body, re.MULTILINE)
    return body[: end.start()] if end else body


def _library_path(stage: str) -> list[str]:
    """The entries of the stage's last LD_LIBRARY_PATH setting."""
    found = re.findall(r"LD_LIBRARY_PATH=(\S+)", stage)
    assert found, "stage sets no LD_LIBRARY_PATH"
    return found[-1].rstrip("\\").split(":")


@pytest.mark.parametrize("kit", KITS)
def test_gpu_runtime_finds_the_staged_libraries(kit: str) -> None:
    """The runtime stage copies /opt/vmafx/lib from its build stage and puts it on the path."""
    text = DOCKERFILE.read_text()
    build = _stage(text, f"{kit}-build")
    runtime = _stage(text, f"{kit}-runtime")
    if "/opt/vmafx/lib/" not in build:
        pytest.skip(f"{kit}-build stages no library in /opt/vmafx/lib")
    assert f"COPY --from={kit}-build /opt/vmafx/lib /opt/vmafx/lib" in runtime
    assert "/opt/vmafx/lib" in _library_path(runtime)


def test_the_library_path_reader_refuses_a_missing_entry() -> None:
    """Negative case: a runtime stage without the directory on its path fails."""
    stage = "ENV LD_LIBRARY_PATH=/opt/vmafx/build/src:/opt/vmafx/lib/rocm \\\n"
    assert "/opt/vmafx/lib" not in _library_path(stage)
    assert _library_path("ENV LD_LIBRARY_PATH=/a:/opt/vmafx/lib \\\n")[-1] == "/opt/vmafx/lib"


def test_the_stage_reader_refuses_an_unknown_stage() -> None:
    """Negative case: asking for a stage the Dockerfile does not define fails."""
    with pytest.raises(AssertionError):
        _stage("FROM x AS a\nRUN true\n", "b")


DEBIAN_COPIES = {"libzimg": "libzimg2", "libgomp": "libgomp1"}


def _uncredited_debian_copies(stage: str) -> list[str]:
    """Debian packages whose library the stage copies without recording its SOURCE and copyright."""
    missing = []
    for lib in sorted(set(re.findall(r"cp -a /usr/lib/\*-linux-gnu\*?/(lib\w+)\.so", stage))):
        package = DEBIAN_COPIES.get(lib, lib)
        source = rf"dpkg-query -W -f='\$\{{source:Package\}}=\$\{{source:Version\}}\\n' {package}\b"
        if not (re.search(source, stage) and f"/usr/share/doc/{package}/copyright" in stage):
            missing.append(package)
    return missing


@pytest.mark.parametrize("stage", ("python-venv", *(f"{kit}-build" for kit in KITS)))
def test_copied_debian_libraries_carry_their_source(stage: str) -> None:
    """A library copied out of a Debian package ships that package's copyright and SOURCE."""
    assert _uncredited_debian_copies(_stage(DOCKERFILE.read_text(), stage)) == []


@pytest.mark.parametrize("kit", KITS)
def test_gpu_runtime_ships_the_libzimg2_record(kit: str) -> None:
    """The runtime stage installs the libzimg2 copyright and SOURCE its licence entry reads."""
    runtime = _stage(DOCKERFILE.read_text(), f"{kit}-runtime")
    assert f"COPY --from={kit}-build /zimg-doc /usr/share/doc/libzimg2" in runtime


def test_an_uncredited_debian_copy_fails() -> None:
    """Negative case: a stage that copies libzimg without its Debian record is reported."""
    stage = "RUN cp -a /usr/lib/*-linux-gnu*/libzimg.so* /opt/vmafx/lib/ \\\n"
    assert _uncredited_debian_copies(stage) == ["libzimg2"]
