# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every image stage that copies a Debian library ships that package's record.

The build stages copy the shared libraries the `vmaf` binary links (libzimg
since zimg became a default dependency) out of Debian's `libzimg2` package. Two
contracts follow, and this module holds both:

- the **tester** image's GPU runtime stages find the libraries their build stage
  staged in `/opt/vmafx/lib`, or the stage's `ldd` check stops the image build
  with `libzimg.so.2 => not found` (the GPU images build only on a push to
  master, so this is the pull-request coverage of that contract);
- every stage that copies such a library also carries the record the licence
  manifest reads — the package's copyright file plus a `dpkg-query` `SOURCE`
  line — and every published runtime stage installs it at
  `/usr/share/doc/libzimg2` (issue #3241 for the tester images, #3242 for the
  production ones).

The copy sites are **discovered** from the tree rather than listed, so a stage
that starts copying a Debian library lands in the contract without anybody
remembering to add it; `KNOWN_COPY_SITES` names the ones the tree has today and
fails when discovery finds one it does not list.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
DOCKERFILE = REPO / "docker/Dockerfile.tester"
KITS = ("sycl", "cuda", "hip")

# Files that can copy a library out of a Debian package into an image tree: the
# Dockerfiles and Containerfiles of the tree, and the shared build scripts a
# builder stage runs (scripts/ci/build-oneapi-libvmaf.sh is the one that stages
# /dist/lib for the two oneAPI images).
COPY_SOURCES = (
    "Dockerfile",
    "Dockerfile.go-server",
    "Containerfile.vmafx",
    "dev/Containerfile",
    "docker/Dockerfile.controller",
    "docker/Dockerfile.node",
    "docker/Dockerfile.production",
    "docker/Dockerfile.production-gpu",
    "docker/Dockerfile.tester",
    "scripts/ci/build-oneapi-libvmaf.sh",
)

# Every stage that copies a Debian library today, as (file, stage, copy verb).
# `vmaf-build` was missing from the pre-#3242 coverage of `Dockerfile.tester`
# even though its /opt/vmafx reaches the published image at `runtime`'s
# `COPY --from=vmaf-build /opt/vmafx /opt/vmafx`; discovery below found it.
KNOWN_COPY_SITES = (
    ("docker/Dockerfile.tester", "python-venv", "cp -a"),
    ("docker/Dockerfile.tester", "vmaf-build", "cp -a"),
    ("docker/Dockerfile.tester", "sycl-build", "cp -a"),
    ("docker/Dockerfile.tester", "cuda-build", "cp -a"),
    ("docker/Dockerfile.tester", "hip-build", "cp -a"),
    ("docker/Dockerfile.production", "builder", "cp -d"),
    ("docker/Dockerfile.controller", "vmaf-builder", "cp -d"),
    ("docker/Dockerfile.production-gpu", "builder-cuda13", "cp -d"),
    ("docker/Dockerfile.production-gpu", "builder-rocm10", "cp -d"),
    ("Dockerfile.go-server", "vmaf-builder", "cp -d"),
    ("docker/Dockerfile.node", "vmaf-builder", "cp -d"),
    ("scripts/ci/build-oneapi-libvmaf.sh", "", "cp -d"),
)

# One copy site is build-only: `vmaf-build` stages /opt/vmafx for the golden
# build profile, and the published `runtime` stage overwrites /opt/vmafx/lib
# from `python-venv` (which does carry the record) before it installs
# /usr/share/doc/libzimg2. `test_a_build_only_copy_site_is_superseded` proves
# that order, so this is an audited exclusion rather than a hole.
BUILD_ONLY_COPY_SITES = (("docker/Dockerfile.tester", "vmaf-build", "cp -a"),)

# The published runtime stages that install the record: (file, stage). The
# install may sit in the stage or in a stage it is FROM.
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

DEBIAN_COPIES = {"libzimg": "libzimg2", "libgomp": "libgomp1"}
DOC_ROOT = "/usr/share/doc"
# Where the shared oneAPI build script writes the record (its `dist_dir`).
ONEAPI_DIST = "/dist"

# A builder that stages the record because a *script* it runs writes it, not a
# COPY/RUN line of its own: (file, stage) -> the script's dist_dir.
SCRIPT_STAGED = {
    ("docker/Dockerfile.production-gpu", "builder-oneapi2026"): ONEAPI_DIST,
    ("docker/Dockerfile.node", "builder-oneapi2026"): ONEAPI_DIST,
}


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
    assert _library_path("ENV LD_LIBRARY_PATH=/opt/vmafx/lib \\\n")[-1] == "/opt/vmafx/lib"


def test_the_stage_reader_refuses_an_unknown_stage() -> None:
    """Negative case: asking for a stage the Dockerfile does not define fails."""
    with pytest.raises(AssertionError):
        _stage("FROM x AS a\nRUN true\n", "b")


COPY_LINE = re.compile(
    r"(?:^|&&|\|\||\n|\bRUN[ \t])\s*(cp -[ad]) /usr/lib/\*-linux-gnu\*?/(lib\w+)\.so", re.MULTILINE
)


def _copied_debian_packages(stage: str) -> list[str]:
    """Debian packages whose library the stage copies out of a /usr/lib directory."""
    return [DEBIAN_COPIES.get(lib, lib) for _verb, lib in sorted(COPY_LINE.findall(stage))]


def _copy_gaps(stage: str) -> list[str]:
    """Debian packages whose library the stage copies without recording its SOURCE and copyright."""
    missing = []
    for _verb, lib in sorted(COPY_LINE.findall(stage)):
        package = DEBIAN_COPIES.get(lib, lib)
        source = rf"dpkg-query -W -f='\$\{{source:Package\}}=\$\{{source:Version\}}\\n' {package}\b"
        if not (re.search(source, stage) and f"{DOC_ROOT}/{package}/copyright" in stage):
            missing.append(package)
    return sorted(set(missing))


def _stages(text: str) -> dict[str, tuple[str, str]]:
    """Stage name -> (parent stage name or base image, body), in file order."""
    found: dict[str, tuple[str, str]] = {}
    headers = list(re.finditer(r"^FROM (\S+) AS (\S+)\n", text, re.MULTILINE))
    for index, match in enumerate(headers):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
        found[match.group(2)] = (match.group(1), text[match.end() : end])
    return found


def _inherit_chain(stages: dict[str, tuple[str, str]], name: str) -> list[tuple[str, str]]:
    """(stage name, body) of stage `name` and of every stage it is FROM, innermost first."""
    chain, current, seen = [], name, set()
    while current in stages and current not in seen:
        seen.add(current)
        parent, body = stages[current]
        chain.append((current, body))
        current = parent
    return chain


def _discovered_copy_sites() -> set[tuple[str, str, str]]:
    """(file, stage, verb) of every stage in the tree that copies a Debian library.

    A Dockerfile is read stage by stage; a shell script (which has no stages) is
    read whole and reported with an empty stage name.
    """
    found: set[tuple[str, str, str]] = set()
    for rel in COPY_SOURCES:
        text = (REPO / rel).read_text(encoding="utf-8")
        stages = _stages(text)
        if stages:
            for stage, (_parent, body) in stages.items():
                for verb, _lib in COPY_LINE.findall(body):
                    found.add((rel, stage, verb))
        else:
            for verb, _lib in COPY_LINE.findall(text):
                found.add((rel, "", verb))
    return found


def test_the_declared_copy_sites_are_every_copy_site_in_the_tree() -> None:
    """Discovery and the declared list agree, so a new copy site cannot slip past.

    This is the check the earlier hard-coded tuple lacked: a stage that starts
    copying a Debian library, or one the list forgot, shows up here by name.
    """
    discovered = _discovered_copy_sites()
    declared = {(path, stage, verb) for path, stage, verb in KNOWN_COPY_SITES}
    assert (
        discovered - declared == set()
    ), "copy sites not covered by KNOWN_COPY_SITES: " + ", ".join(
        f"{p}:{s} ({v})" for p, s, v in sorted(discovered - declared)
    )
    assert (
        declared - discovered == set()
    ), "KNOWN_COPY_SITES names stages that copy no Debian library any more: " + ", ".join(
        f"{p}:{s} ({v})" for p, s, v in sorted(declared - discovered)
    )


def test_the_copy_site_discovery_finds_a_planted_site() -> None:
    """Known gap: discovery reads COPY_SOURCES, so a Dockerfile outside it is not seen.

    The list is the contract: adding a Dockerfile that copies a Debian library
    means adding it to COPY_SOURCES (or the site goes unheld).
    """
    assert "docker/Dockerfile.tester" in COPY_SOURCES
    assert "docker/Dockerfile.production-gpu" in COPY_SOURCES
    assert "docker/Dockerfile.operator" not in COPY_SOURCES  # copies nothing from /usr/lib


@pytest.mark.parametrize(
    "path,stage,verb",
    [site for site in KNOWN_COPY_SITES if site not in BUILD_ONLY_COPY_SITES],
    ids=[
        f"{Path(path).name}:{stage or 'script'}"
        for path, stage, _verb in KNOWN_COPY_SITES
        if (path, stage, _verb) not in BUILD_ONLY_COPY_SITES
    ],
)
def test_copied_debian_libraries_carry_their_source(path: str, stage: str, verb: str) -> None:
    """A library copied out of a Debian package ships that package's copyright and SOURCE.

    The non-empty check keeps the parametrization honest: a stage that stops
    copying the library would otherwise pass while asserting nothing.
    """
    text = (REPO / path).read_text(encoding="utf-8")
    body = _stage(text, stage) if stage else text
    assert verb in body, f"{path}:{stage} has no '{verb}' copy line; update KNOWN_COPY_SITES"
    assert "libzimg2" in _copied_debian_packages(
        body
    ), f"{path}:{stage or 'script'} copies no libzimg2"
    assert _copy_gaps(body) == []


def test_a_build_only_copy_site_is_superseded() -> None:
    """The one build-only copy site's library is replaced, with its record, in the image.

    `vmaf-build` copies libzimg into /opt/vmafx/lib without a doc record. The
    published `runtime` stage copies /opt/vmafx from it and then overwrites
    /opt/vmafx/lib from `python-venv` — which does stage the record — before
    installing /usr/share/doc/libzimg2. Docker's COPY order is the proof: the
    later COPY of the same destination wins, so the image holds python-venv's
    libzimg, whose record is installed at line 156. If either half of that
    pairing changes (the record source, or the lib overwrite), this fails.
    """
    text = DOCKERFILE.read_text(encoding="utf-8")
    runtime = _stage(text, "runtime")
    lib_source = re.search(r"^COPY --from=(\S+) (\S+) /opt/vmafx/lib$", runtime, re.MULTILINE)
    assert lib_source, "runtime no longer copies /opt/vmafx/lib from a stage"
    winner, source_dir = lib_source.groups()
    assert source_dir == "/extra-libs/lib", source_dir
    assert winner == "python-venv", winner
    # The stage that wins must itself carry the record, and the runtime installs it.
    venv = _stage(text, winner)
    assert _copy_gaps(venv) == []
    assert f"COPY --from={winner} /extra-libs/doc/libzimg2 {DOC_ROOT}/libzimg2" in runtime
    # And it must be copied at all, so the build-only copy is superseded and not dropped.
    whole_tree = "COPY --from=vmaf-build /opt/vmafx /opt/vmafx"
    assert whole_tree in runtime
    assert runtime.index(whole_tree) < runtime.index(
        f"COPY --from={winner} {source_dir} /opt/vmafx/lib"
    )


@pytest.mark.parametrize("verb", ("cp -a", "cp -d"))
def test_an_uncredited_debian_copy_fails(verb: str) -> None:
    """Negative case: a stage that copies a Debian library without its record is reported."""
    stage = f"RUN {verb} /usr/lib/*-linux-gnu*/libzimg.so* /somewhere/lib/ \\\n"
    assert _copy_gaps(stage) == ["libzimg2"]


def test_the_copy_reader_refuses_a_record_for_another_package() -> None:
    """Negative case: recording libgomp1's source does not credit a copied libzimg2."""
    stage = (
        "RUN cp -d /usr/lib/*-linux-gnu*/libzimg.so* /dist/lib/ \\\n"
        "    && dpkg-query -W -f='${source:Package}=${source:Version}\\n' libgomp1 \\\n"
        "        > /dist/doc/libgomp1/SOURCE \\\n"
        "    && cp /usr/share/doc/libgomp1/copyright /dist/doc/libgomp1/copyright\n"
    )
    assert _copy_gaps(stage) == ["libzimg2"]


def test_the_copy_reader_sees_the_forms_the_tree_uses() -> None:
    """The forms the reader models, and the one it does not, recorded rather than implied.

    It reads `cp -a` / `cp -d` from a `/usr/lib/*-linux-gnu*/` glob — every form
    in COPY_SOURCES today, whether the line starts the `RUN` or continues its
    `&&` chain. A path spelled with a concrete arch (`/usr/lib/x86_64-linux-gnu`)
    or built in a shell variable, and `install` instead of `cp`, are not
    modelled; that is why discovery and the declared list are compared, and why
    a new copy written that way has to be added to `COPY_SOURCES` and this
    reader together.
    """
    assert _copy_gaps("RUN cp -a /usr/lib/*-linux-gnu*/libzimg.so* /x \\\n") == ["libzimg2"]
    assert _copy_gaps("RUN cp -d /usr/lib/*-linux-gnu*/libzimg.so* /x \\\n") == ["libzimg2"]
    assert _copy_gaps("RUN cp -a /usr/lib/*-linux-gnu/libzimg.so* /x \\\n") == ["libzimg2"]
    assert _copy_gaps(
        "RUN mkdir -p /x \\\n    && cp -a /usr/lib/*-linux-gnu*/libzimg.so* /x/ \\\n"
    ) == ["libzimg2"]
    # Known gap, deliberate: a concrete arch path and an `install` call are unseen.
    assert _copy_gaps("RUN cp -a /usr/lib/x86_64-linux-gnu/libzimg.so* /x \\\n") == []
    assert _copy_gaps("RUN install -m 644 /usr/lib/*-linux-gnu*/libzimg.so.2 /x \\\n") == []


@pytest.mark.parametrize("kit", KITS)
def test_gpu_runtime_ships_the_libzimg2_record(kit: str) -> None:
    """The runtime stage copies the record its build stage wrote, into the image."""
    runtime = _stage(DOCKERFILE.read_text(), f"{kit}-runtime")
    installs = _record_installs(runtime)
    assert installs, f"{kit}-runtime installs no {DOC_ROOT}/libzimg2"


def _record_installs(body: str) -> list[tuple[str, str]]:
    """(source, builder) of every `COPY --from=<builder> <source> /usr/share/doc/libzimg2`."""
    pattern = rf"^COPY --from=(\S+) (\S+) {re.escape(DOC_ROOT)}/libzimg2/?[ \t]*$"
    return [(source, builder) for builder, source in re.findall(pattern, body, re.MULTILINE)]


def _builder_writes_record(path: str, builder: str, source: str) -> bool:
    """Whether stage `builder` of `path` writes the record under `source`.

    A stage writes it either with a COPY/RUN line of its own, or by running a
    shared script that writes to the stage's dist_dir — `SCRIPT_STAGED` names
    those, so `builder-oneapi2026` (which only runs
    scripts/ci/build-oneapi-libvmaf.sh) is not held to a line it does not have.
    """
    staged = _stages((REPO / path).read_text(encoding="utf-8")).get(builder)
    if staged is None:
        return False
    body = "\n".join(
        line for line in staged[1].splitlines() if not line.lstrip().startswith("#")
    )
    root = re.escape(source.rstrip("/"))
    writes_copyright = re.search(rf"\bcp\s+\S+/copyright\s+{root}/copyright(?=\s|$)", body)
    writes_source = re.search(rf">\s*{root}/SOURCE(?=\s|$)", body)
    if writes_copyright and writes_source:
        return True
    dist = SCRIPT_STAGED.get((path, builder))
    return bool(dist) and source.rstrip("/") == f"{dist}/doc/libzimg2"


@pytest.mark.parametrize(
    "path,stage",
    RUNTIME_STAGES,
    ids=[f"{Path(path).name}:{stage}" for path, stage in RUNTIME_STAGES],
)
def test_a_published_runtime_installs_the_record(path: str, stage: str) -> None:
    """The runtime stage copies the record from the builder that staged it.

    The install must be a real `COPY --from=<stage> <dir> /usr/share/doc/libzimg2`
    in the stage or in a stage it inherits, AND that stage must write `<dir>` —
    a comment or an `ENV` naming the path does not satisfy this, and neither
    does a COPY whose source the builder never creates.
    """
    stages = _stages((REPO / path).read_text(encoding="utf-8"))
    for current, body in _inherit_chain(stages, stage):
        for source, builder in _record_installs(body):
            assert (
                builder in stages
            ), f"{path}:{current} copies the record from unknown stage {builder}"
            assert _builder_writes_record(
                path, builder, source
            ), f"{path}:{builder} never writes {source}, which {current} copies"
            return
    raise AssertionError(f"{path}:{stage} installs no {DOC_ROOT}/libzimg2")


def test_the_record_install_reader_refuses_a_comment_or_env() -> None:
    """Negative cases: naming the path is not installing the record."""
    assert _record_installs("# we used to COPY /dist/doc/libzimg2 /usr/share/doc/libzimg2\n") == []
    assert _record_installs("ENV X=/usr/share/doc/libzimg2\n") == []
    assert _record_installs("RUN mkdir -p /usr/share/doc/libzimg2\n") == []
    assert _record_installs("COPY --from=builder /dist/doc/libzimg2 /usr/share/doc/libzimg2\n") == [
        ("/dist/doc/libzimg2", "builder")
    ]
    assert _record_installs(
        "COPY --from=builder /dist/doc/libzimg2 /usr/share/doc/libzimg2/\n"
    ) == [("/dist/doc/libzimg2", "builder")]


def test_the_record_install_reader_refuses_a_neighbouring_target() -> None:
    """Negative cases: a sibling or a child path is not the record directory."""
    assert (
        _record_installs("COPY --from=b /dist/doc/libzimg2 /usr/share/doc/libzimg2-backup\n") == []
    )
    assert (
        _record_installs("COPY --from=b /dist/doc/libzimg2 /usr/share/doc/libzimg2/SOURCE\n") == []
    )


def test_the_record_writer_reader_needs_both_files(tmp_path: Path) -> None:
    """Negative cases: creating the directory, or writing the files elsewhere, is no record."""
    writes = (
        "    && cp /usr/share/doc/libzimg2/copyright /dist/doc/libzimg2/copyright \\\n"
        "    && dpkg-query -W -f='${source:Package}=${source:Version}\\n' libzimg2"
        " > /dist/doc/libzimg2/SOURCE\n"
    )
    cases = {
        "complete": (writes, True),
        "mkdir-only": ("    && mkdir -p /dist/doc/libzimg2/\n", False),
        "comment-only": (
            "# cp x/copyright /dist/doc/libzimg2/copyright > /dist/doc/libzimg2/SOURCE\n",
            False,
        ),
        "elsewhere": (writes.replace("/dist/doc/libzimg2/", "/tmp/"), False),
        "copyright-only": (writes.splitlines()[0] + "\n", False),
    }
    for name, (body, expected) in cases.items():
        dockerfile = tmp_path / f"Dockerfile.{name}"
        dockerfile.write_text(f"FROM base AS b\nRUN true \\\n{body}", encoding="utf-8")
        got = _builder_writes_record(str(dockerfile), "b", "/dist/doc/libzimg2")
        assert got is expected, name


def test_node_sycl_takes_the_record_with_its_libraries() -> None:
    """node-sycl-assembled replaces libzimg.so* from builder-oneapi2026, so its record too."""
    stage = _stage(
        (REPO / "docker/Dockerfile.node").read_text(encoding="utf-8"), "node-sycl-assembled"
    )
    assert "COPY --from=builder-oneapi2026 /dist/lib/" in stage
    assert _record_installs(stage) == [("/dist/doc/libzimg2", "builder-oneapi2026")]


def test_the_inheritance_reader_sees_a_parent_stage() -> None:
    """Negative case: an install by the FROM stage counts, an absent one does not."""
    text = (
        "FROM base AS parent\n"
        "COPY --from=b /dist/doc/libzimg2 /usr/share/doc/libzimg2\n"
        "FROM parent AS child\n"
        "RUN true\n"
    )
    stages = _stages(text)
    assert [name for name, _ in _inherit_chain(stages, "child")] == ["child", "parent"]
    assert [name for name, _ in _inherit_chain(stages, "absent")] == []


def test_the_oneapi_build_script_records_the_libzimg2_it_stages() -> None:
    """The shared oneAPI build script stages libzimg.so*, so it stages the record too.

    Two published images take /dist/lib from builder-oneapi2026: the production
    oneAPI image and the node SYCL image. The `builder-oneapi2026` stages carry
    no copy line of their own — they run this script — so the script is the copy
    site, and it is in `COPY_SOURCES` so discovery sees it.
    """
    script = (REPO / "scripts/ci/build-oneapi-libvmaf.sh").read_text(encoding="utf-8")
    assert _copied_debian_packages(script) == ["libzimg2"]
    assert _copy_gaps(script) == []
    assert "dpkg-query" in script and f"{DOC_ROOT}/libzimg2/copyright" in script


def test_the_oneapi_stages_stage_no_library_themselves() -> None:
    """The two builder-oneapi2026 stages run the shared script and copy nothing themselves."""
    for path in ("docker/Dockerfile.production-gpu", "docker/Dockerfile.node"):
        body = _stage((REPO / path).read_text(encoding="utf-8"), "builder-oneapi2026")
        assert _copied_debian_packages(body) == [], path
        assert "build-oneapi-libvmaf.sh" in body, path
