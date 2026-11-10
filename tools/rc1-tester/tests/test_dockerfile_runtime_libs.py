# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every image stage that copies a Debian library ships that package's record.

The GPU build stages of `docker/Dockerfile.tester` copy the shared libraries the
`vmaf` binary links (libzimg since zimg became a default dependency) into
`/opt/vmafx/lib`. The matching runtime stage must copy that directory and put it
on `LD_LIBRARY_PATH`, or the stage's `ldd` check stops the image build with
`libzimg.so.2 => not found`. The GPU images build only on a push to master, so
this test is the pull-request coverage of that contract.

The same stages copy `libzimg.so*` out of Debian's `libzimg2` package, so they
also carry the record the licence manifest reads (issue #3241 for the tester
images, issue #3242 for the production ones): the package's copyright file plus
a `dpkg-query` `SOURCE` line. One table names every copying builder and its
published runtime stages, so a new copy site cannot land without the record
(HISS-19).
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
    assert "/opt/vmafx/lib/" in build, f"{kit}-build stages no library in /opt/vmafx/lib"
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

# Every image stage that copies a Debian library into a tree a published image is
# built from, as (file, stage, copy verb). The verb is `cp -a` in the tester
# image's build stages (whole SONAME chain followed) and `cp -d` in the
# production builders (the symlinks are copied as symlinks). Issue #3242's scope
# note of 2026-11-10 named six production builder lines; `Dockerfile.go-server`
# and `docker/Dockerfile.node` are the two the issue text had missed, and the
# node image inherits `production-cli-image`'s zimg component like the rest.
COPY_SITES = (
    ("docker/Dockerfile.tester", "python-venv", "cp -a"),
    ("docker/Dockerfile.tester", "sycl-build", "cp -a"),
    ("docker/Dockerfile.tester", "cuda-build", "cp -a"),
    ("docker/Dockerfile.tester", "hip-build", "cp -a"),
    ("docker/Dockerfile.production", "builder", "cp -d"),
    ("docker/Dockerfile.controller", "vmaf-builder", "cp -d"),
    ("docker/Dockerfile.production-gpu", "builder-cuda13", "cp -d"),
    ("docker/Dockerfile.production-gpu", "builder-rocm10", "cp -d"),
    ("Dockerfile.go-server", "vmaf-builder", "cp -d"),
    ("docker/Dockerfile.node", "vmaf-builder", "cp -d"),
)

# The published runtime stages that install the record, each with the base of its
# FROM chain: the record may come from the stage itself or from a stage it is
# FROM (node-sycl-assembled is FROM node-assembled, which is FROM runtime-base).
RUNTIME_STAGES = (
    ("docker/Dockerfile.tester", "runtime"),
    ("docker/Dockerfile.tester", "sycl-runtime"),
    ("docker/Dockerfile.tester", "cuda-runtime"),
    ("docker/Dockerfile.tester", "hip-runtime"),
    ("docker/Dockerfile.production", "cli-assembled"),
    ("docker/Dockerfile.production", "server-assembled"),
    ("docker/Dockerfile.controller", "controller-assembled"),
    ("docker/Dockerfile.production-gpu", "cuda13-assembled"),
    ("docker/Dockerfile.production-gpu", "rocm10-assembled"),
    ("docker/Dockerfile.production-gpu", "oneapi2026-assembled"),
    ("Dockerfile.go-server", "go-server-assembled"),
    ("docker/Dockerfile.node", "node-assembled"),
    ("docker/Dockerfile.node", "node-sycl-assembled"),
)


def _copy_gaps(stage: str, verb: str) -> list[str]:
    """Debian packages whose library the stage copies without recording its SOURCE and copyright."""
    pattern = rf"{re.escape(verb)} /usr/lib/\*-linux-gnu\*?/(lib\w+)\.so"
    missing = []
    for lib in sorted(set(re.findall(pattern, stage))):
        package = DEBIAN_COPIES.get(lib, lib)
        source = rf"dpkg-query -W -f='\$\{{source:Package\}}=\$\{{source:Version\}}\\n' {package}\b"
        if not (re.search(source, stage) and f"/usr/share/doc/{package}/copyright" in stage):
            missing.append(package)
    return missing


def _stages(text: str) -> dict[str, tuple[str, str]]:
    """Stage name -> (parent stage name or base image, body), in file order."""
    found: dict[str, tuple[str, str]] = {}
    headers = list(re.finditer(r"^FROM (\S+) AS (\S+)\n", text, re.MULTILINE))
    for index, match in enumerate(headers):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
        found[match.group(2)] = (match.group(1), text[match.end() : end])
    return found


def _inherited(stages: dict[str, tuple[str, str]], name: str) -> str:
    """The bodies of stage `name` and of every stage it is FROM, joined."""
    bodies, current, seen = [], name, set()
    while current in stages and current not in seen:
        seen.add(current)
        parent, body = stages[current]
        bodies.append(body)
        current = parent
    return "".join(bodies)


@pytest.mark.parametrize(
    "path,stage,verb",
    COPY_SITES,
    ids=[f"{Path(path).name}:{stage}" for path, stage, _verb in COPY_SITES],
)
def test_copied_debian_libraries_carry_their_source(path: str, stage: str, verb: str) -> None:
    """A library copied out of a Debian package ships that package's copyright and SOURCE."""
    text = (REPO / path).read_text(encoding="utf-8")
    assert _copy_gaps(_stage(text, stage), verb) == []


@pytest.mark.parametrize("verb", ("cp -a", "cp -d"))
def test_an_uncredited_debian_copy_fails(verb: str) -> None:
    """Negative case: a stage that copies a Debian library without its record is reported."""
    stage = f"RUN {verb} /usr/lib/*-linux-gnu*/libzimg.so* /somewhere/lib/ \\\n"
    assert _copy_gaps(stage, verb) == ["libzimg2"]


def test_the_copy_reader_refuses_a_stage_with_a_record_for_another_package() -> None:
    """Negative case: recording libgomp1's source does not credit a copied libzimg2."""
    stage = (
        "RUN cp -d /usr/lib/*-linux-gnu*/libzimg.so* /dist/lib/ \\\n"
        "    && dpkg-query -W -f='${source:Package}=${source:Version}\\n' libgomp1 \\\n"
        "        > /dist/doc/libgomp1/SOURCE \\\n"
        "    && cp /usr/share/doc/libgomp1/copyright /dist/doc/libgomp1/copyright\n"
    )
    assert _copy_gaps(stage, "cp -d") == ["libzimg2"]


@pytest.mark.parametrize("kit", KITS)
def test_gpu_runtime_ships_the_libzimg2_record(kit: str) -> None:
    """The runtime stage installs the libzimg2 copyright and SOURCE its licence entry reads."""
    runtime = _stage(DOCKERFILE.read_text(), f"{kit}-runtime")
    assert f"COPY --from={kit}-build /zimg-doc /usr/share/doc/libzimg2" in runtime


@pytest.mark.parametrize(
    "path,stage",
    RUNTIME_STAGES,
    ids=[f"{Path(path).name}:{stage}" for path, stage in RUNTIME_STAGES],
)
def test_a_published_runtime_installs_the_record(path: str, stage: str) -> None:
    """The runtime stage carries the copyright and SOURCE its licence entry reads."""
    tree = _inherited(_stages((REPO / path).read_text(encoding="utf-8")), stage)
    assert "/doc/libzimg2" in tree, "no builder staging of the record"
    assert "/usr/share/doc/libzimg2" in tree, "the runtime stage does not install it"


def test_the_inheritance_reader_sees_a_parent_stage() -> None:
    """Negative case: a record installed by the FROM stage counts, an absent one does not."""
    text = (
        "FROM base AS parent\n"
        "COPY --from=b /dist/doc/libzimg2 /usr/share/doc/libzimg2\n"
        "FROM parent AS child\n"
        "RUN true\n"
    )
    stages = _stages(text)
    assert "/usr/share/doc/libzimg2" in _inherited(stages, "child")
    assert "/usr/share/doc/libzimg2" in _inherited(stages, "parent")
    assert _inherited(stages, "absent") == ""
    without = "FROM base AS parent\nRUN true\nFROM parent AS child\nRUN true\n"
    assert "/usr/share/doc/libzimg2" not in _inherited(_stages(without), "child")


def test_the_oneapi_build_script_records_the_libzimg2_it_stages() -> None:
    """The shared oneAPI build script stages libzimg.so*, so it stages the record too.

    Two published images take /dist/lib from builder-oneapi2026: the production
    oneAPI image and the node SYCL image. Both inherit the zimg component, so the
    script is a copy site like the Dockerfiles above and is held to the same rule.
    """
    script = (REPO / "scripts/ci/build-oneapi-libvmaf.sh").read_text(encoding="utf-8")
    assert _copy_gaps(script, "cp -d") == []
    assert "dpkg-query" in script and "/usr/share/doc/libzimg2/copyright" in script


def test_a_oneapi_build_script_without_the_record_fails() -> None:
    """Negative case: the oneAPI staging without the Debian record is reported."""
    script = 'cp -d /usr/lib/*-linux-gnu*/libzimg.so* "${dist_dir}/lib/"\n'
    assert _copy_gaps(script, "cp -d") == ["libzimg2"]
