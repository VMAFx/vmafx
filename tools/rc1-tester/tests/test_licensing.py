# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the licence record, notices and gate of the tester artifacts (ADR-1503)."""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import re
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest

_IMAGE = Path(__file__).resolve().parents[1] / "image"
_spec = importlib.util.spec_from_file_location("licensing", _IMAGE / "licensing.py")
lic = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lic)

REPO = Path(__file__).resolve().parents[3]
BUILD_ID = "ab" * 20
TAG = "SPDX-" + "License-Identifier:"


# ------------------------------------------------------------------ helpers


def make_elf(build_id: str) -> bytes:
    """A minimal 64-bit little-endian ELF with one SHT_NOTE section holding a GNU build ID."""
    desc = bytes.fromhex(build_id)
    note = struct.pack("<III", 4, len(desc), 3) + b"GNU\0" + desc
    shoff = 64
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HH", header, 0x3A, 64, 2)
    null_section = bytes(64)
    section = bytearray(64)
    struct.pack_into("<I", section, 4, 7)
    struct.pack_into("<QQ", section, 0x18, shoff + 128, len(note))
    return bytes(header) + null_section + bytes(section) + note


def record_line(path: Path, rel: str) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(path.read_bytes()).digest()).decode()
    return f"{rel},sha256={digest.rstrip('=')},{path.stat().st_size}"


def write(path: Path, text: str | bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text) if isinstance(text, bytes) else path.write_text(text)
    return path


def fake_repo(tmp: Path) -> Path:
    repo = tmp / "repo"
    write(repo / "REUSE.toml", 'version = 1\n[[annotations]]\npath = ["data/**"]\n'
          'precedence = "closest"\nSPDX-FileCopyrightText = "2020 Data Owner"\n'
          'SPDX-License-Identifier = "MIT"\n')  # fmt: skip
    write(repo / "LICENSES/MIT.txt", "MIT text\n")
    write(repo / "LICENSES/EUPL-1.2.txt", "EUPL text\n")
    write(repo / "tools/report.py", f"# Copyright 2026 Lusoris\n# {TAG} EUPL-1.2\n")
    write(repo / "data/table.json", "{}\n")
    write(repo / "fixtures.sha256", "00  clip.yuv\n")
    return repo


def manifest() -> dict:
    return {
        "spdx_texts": {"MIT": "LICENSES/MIT.txt", "EUPL-1.2": "LICENSES/EUPL-1.2.txt"},
        "cpython_license_rst": {},
        "generated_build_files": [{"pattern": "src/config.h", "licence": "NONE"}],
        "grafted_libraries": [
            {"pattern": "libquadmath-*.so*", "licence": "LGPL-2.1-or-later", "copyleft": True,
             "sources": {BUILD_ID: "gcc-src"}},
            {"pattern": "libopenblas-*.so", "licence": "BSD-3-Clause", "copyleft": False},
        ],
        "source_archives": {"gcc-src": {"file": "gcc.src.rpm", "url": "https://example.invalid/gcc.src.rpm",
                                        "sha256": "0" * 64, "why": "test"}},
        "artifacts": {"kit": record()},
    }  # fmt: skip


def record() -> dict:
    return {
        "title": "the test kit", "licence_root": "licenses", "source_offer": "the image {tag}-source",
        "python": {"version": "3.14.8"},
        "components": [
            {"id": "bins", "kind": "build", "name": "binaries", "paths": ["bin/**"],
             "licences": ["EUPL-1.2", "MIT"]},
            {"id": "videos", "kind": "fixed", "name": "videos", "licence": "MIT",
             "manifest": {"file": "fixtures.sha256", "prefix": "res/"}},
            {"id": "files", "kind": "repo", "name": "repository files", "licences": ["EUPL-1.2", "MIT"],
             "map": [{"artifact": "tester/", "repo": "tools/"}, {"artifact": "data/", "repo": "data/"}]},
            {"id": "py", "kind": "python-dist", "name": "packages", "roots": ["site"]},
            {"id": "deb", "kind": "dpkg", "name": "packages"},
            {"id": "state", "kind": "state", "name": "state", "paths": ["var/lib/dpkg/**"]},
            {"id": "notices", "kind": "notices", "name": "notices", "paths": ["licenses/**"]},
        ],
    }  # fmt: skip


def fake_artifact(tmp: Path) -> Path:
    root = tmp / "root"
    write(root / "bin/vmaf", "binary")
    write(root / "res/clip.yuv", "yuv")
    write(root / "tester/report.py", f"# Copyright 2026 Lusoris\n# {TAG} EUPL-1.2\n")
    write(root / "data/table.json", "{}\n")
    write(root / "var/lib/dpkg/status", "Package: tar\nStatus: install ok installed\n"
          "Version: 1.35-1\nSource: tar (1.35+dfsg-1)\nBuilt-Using: glibc (= 2.41-1)\n\n")  # fmt: skip
    write(
        root / "var/lib/dpkg/info/tar.list",
        "/.\n/usr\n/usr/bin/tar\n/usr/share/doc/tar/copyright\n",
    )
    write(root / "usr/bin/tar", "tar")
    write(root / "usr/share/doc/tar/copyright", "GPL-3+\n")
    dist = root / "site/pkg-1.0.dist-info"
    lib = write(root / "site/pkg.libs/libquadmath-1234.so.0", make_elf(BUILD_ID))
    write(root / "site/pkg/__init__.py", "")
    write(
        dist / "METADATA",
        "Metadata-Version: 2.4\nName: pkg\nVersion: 1.0\nLicense-Expression: MIT\n",
    )
    write(dist / "licenses/LICENSE", "MIT text\n")
    rows = [record_line(root / "site/pkg/__init__.py", "pkg/__init__.py"),
            record_line(lib, "pkg.libs/libquadmath-1234.so.0"),
            "pkg-1.0.dist-info/METADATA,,", "pkg-1.0.dist-info/RECORD,,",
            "pkg-1.0.dist-info/licenses/LICENSE,,"]  # fmt: skip
    write(dist / "RECORD", "\n".join(rows) + "\n")
    return root


def scan(licences: tuple[str, ...] = ("EUPL-1.2",)) -> dict:
    files = [{"path": f"core/f{i}.c", "licence": e, "copyright": ["Copyright 2026 Lusoris"]}
             for i, e in enumerate(licences)]  # fmt: skip
    return {"schema_version": 1, "licences": sorted(licences), "files": files, "system_inputs": 0}


def setup_tree(tmp: Path, licences: tuple[str, ...] = ("EUPL-1.2",)) -> argparse.Namespace:
    repo, root = fake_repo(tmp), fake_artifact(tmp)
    scan_path = write(tmp / "scan.json", json.dumps(scan(licences)))
    (tmp / "texts").mkdir()
    return argparse.Namespace(artifact="kit", root=str(root), repo=str(repo), build_scan=str(scan_path),
                              texts=str(tmp / "texts"), source_commit="c0ffee", tag="t1",
                              python_version="3.14.8", receipt=None)  # fmt: skip


def notices_then_check(args: argparse.Namespace, data: dict | None = None) -> list[str]:
    data = data or manifest()
    lic.write_notices(args, data)
    return lic.run_check(args, data)


# ------------------------------------------------------------- expressions


def test_spdx_ids_split_operators_and_parentheses() -> None:
    expr = "(EUPL-1.2 AND BSD-2-Clause) OR GPL-3.0-or-later WITH GCC-exception-3.1"
    assert lic.spdx_ids(expr) == {
        "EUPL-1.2",
        "BSD-2-Clause",
        "GPL-3.0-or-later",
        "GCC-exception-3.1",
    }
    assert lic.spdx_ids("") == set()


def test_clean_expression_drops_comment_closers() -> None:
    assert lic.clean_expression(" BSD-2-Clause-Patent */") == "BSD-2-Clause-Patent"
    assert lic.clean_expression('EUPL-1.2\\n",') == "EUPL-1.2"


def test_reuse_glob_star_stays_in_one_directory() -> None:
    assert lic.reuse_glob("model/*").match("model/a.json")
    assert not lic.reuse_glob("model/*").match("model/x/a.json")
    assert lic.reuse_glob("model/**").match("model/x/a.json")
    assert lic.reuse_glob("a\\*b").match("a*b") and not lic.reuse_glob("a\\*b").match("axb")


def test_clean_copyright_needs_a_year_or_mark() -> None:
    assert lic.clean_copyright('Copyright 2016, Netflix, Inc."') == "Copyright 2016, Netflix, Inc."
    assert lic.clean_copyright("copyright notice in its entirety") == ""


def test_file_licence_header_wins_unless_override(tmp_path: Path) -> None:
    repo = fake_repo(tmp_path)
    path = write(repo / "data/own.py", f"# Copyright 2026 Lusoris\n# {TAG} EUPL-1.2\n")
    reuse = lic.load_reuse(repo)
    assert lic.file_licence(path, "data/own.py", reuse)[0] == "EUPL-1.2"
    assert lic.file_licence(repo / "data/table.json", "data/table.json", reuse) == (
        "MIT",
        ["Copyright 2020 Data Owner"],
    )
    reuse[0]["precedence"] = "override"
    assert lic.file_licence(path, "data/own.py", reuse)[0] == "MIT"


def test_elf_build_id_reads_the_gnu_note(tmp_path: Path) -> None:
    assert lic.elf_build_id(write(tmp_path / "lib.so", make_elf(BUILD_ID))) == BUILD_ID
    assert lic.elf_build_id(write(tmp_path / "text.so", b"not an elf at all, just text")) is None


# ---------------------------------------------------------------- the gate


def test_a_recorded_tree_passes_and_carries_its_texts(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    assert notices_then_check(args) == []
    licences = Path(args.root) / "licenses"
    text = (licences / lic.NOTICES_NAME).read_text()
    assert "https://github.com/VMAFx/vmafx/tree/c0ffee" in text and "the image t1-source" in text
    assert "  tar 1.35-1  source tar=1.35+dfsg-1 glibc=2.41-1" in text
    assert "libquadmath-1234.so.0: LGPL-2.1-or-later; source gcc.src.rpm" in text
    assert (licences / "texts/EUPL-1.2.txt").read_text() == "EUPL text\n"
    assert (licences / "texts/MIT.txt").is_file()  # the repo-mapped data file is MIT


def test_an_unrecorded_file_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    write(Path(args.root) / "opt/vendor/libsecret.so", "x")
    assert "no recorded licence: opt/vendor/libsecret.so" in notices_then_check(args)


def test_a_package_without_its_copyright_file_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    (Path(args.root) / "usr/share/doc/tar/copyright").unlink()
    problems = notices_then_check(args)
    assert "package tar has no /usr/share/doc/*/copyright" in problems


def test_a_dist_info_without_licence_file_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    (Path(args.root) / "site/pkg-1.0.dist-info/licenses/LICENSE").unlink()
    assert "pkg-1.0.dist-info keeps no licence file" in notices_then_check(args)


def test_a_modified_grafted_library_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    lib = Path(args.root) / "site/pkg.libs/libquadmath-1234.so.0"
    lib.write_bytes(lib.read_bytes() + b"\0")  # what `strip` does to a vendor binary
    problems = notices_then_check(args)
    assert any("libquadmath-1234.so.0 differs from its wheel RECORD" in p for p in problems)


def test_copyleft_graft_without_recorded_source_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    data = manifest()
    data["grafted_libraries"][0]["sources"] = {}
    problems = notices_then_check(args, data)
    assert any("has no recorded source" in p and BUILD_ID in p for p in problems)


def test_unknown_grafted_library_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    data = manifest()
    data["grafted_libraries"] = data["grafted_libraries"][1:]
    assert any("has no grafted_libraries entry" in p for p in notices_then_check(args, data))


def test_a_compiled_licence_outside_the_record_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path, licences=("EUPL-1.2", "MIT", "GPL-2.0-only"))
    data = manifest()
    data["spdx_texts"]["GPL-2.0-only"] = "LICENSES/MIT.txt"
    problems = notices_then_check(args, data)
    assert any("GPL-2.0-only, not in component bins" in p for p in problems)


def test_a_repository_file_with_another_licence_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    write(Path(args.root) / "tester/vendored.py", f"# {TAG} GPL-3.0-only\n")
    data = manifest()
    data["spdx_texts"]["GPL-3.0-only"] = "LICENSES/MIT.txt"
    problems = notices_then_check(args, data)
    assert any("tester/vendored.py declares GPL-3.0-only" in p for p in problems)


def test_a_licence_without_text_stops_the_notices(tmp_path: Path) -> None:
    args = setup_tree(tmp_path, licences=("EUPL-1.2", "Unlisted-1.0"))
    with pytest.raises(lic.LicensingError, match="Unlisted-1.0 has no text"):
        lic.write_notices(args, manifest())


def test_another_interpreter_version_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    args.python_version = "3.14.9"
    assert any("Python 3.14.9" in p for p in notices_then_check(args))


def test_missing_or_stale_notices_fail(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    assert any("THIRD_PARTY_NOTICES.txt is missing" in p for p in lic.run_check(args, manifest()))
    lic.write_notices(args, manifest())
    status = Path(args.root) / "var/lib/dpkg/status"
    status.write_text(status.read_text().replace("1.35-1\n", "1.36-1\n", 1))
    assert "the notices do not list package tar" in lic.run_check(args, manifest())


def test_report_check_exit_status_and_receipt(tmp_path: Path, capsys) -> None:
    args = argparse.Namespace(
        artifact="kit", python_version="3.14.8", receipt=str(tmp_path / "r.json")
    )
    assert lic.report_check(args, ["no recorded licence: x"]) == 1
    assert not (tmp_path / "r.json").exists()
    assert lic.report_check(args, []) == 0
    assert json.loads((tmp_path / "r.json").read_text())["result"] == "pass"
    assert "no recorded licence: x" in capsys.readouterr().err


# -------------------------------------------------------------- scan-build


def fake_build(tmp: Path, monkeypatch, generated: str = "src/config.h") -> tuple[Path, Path]:
    repo = fake_repo(tmp)
    write(repo / "core/a.c", f"/* Copyright 2019 Someone\n * {TAG} MIT */\n")
    build = tmp / "build"
    write(build / generated, "#define X 1\n")
    deps = ["../repo/core/a.c", generated, "/usr/include/stdio.h"]
    monkeypatch.setattr(lic, "ninja_deps", lambda _build: deps)
    return build, repo


def test_scan_build_reads_compiled_files_and_generated_rules(tmp_path: Path, monkeypatch) -> None:
    build, repo = fake_build(tmp_path, monkeypatch)
    result = lic.scan_build(build, repo, manifest())
    assert result["licences"] == ["MIT"]
    assert {
        "path": "core/a.c",
        "licence": "MIT",
        "copyright": ["Copyright 2019 Someone"],
    } in result["files"]
    assert result["system_inputs"] == 1


def test_scan_build_refuses_an_unruled_generated_file(tmp_path: Path, monkeypatch) -> None:
    build, repo = fake_build(tmp_path, monkeypatch, generated="src/blob.c")
    with pytest.raises(
        lic.LicensingError, match="src/blob.c matches no generated_build_files rule"
    ):
        lic.scan_build(build, repo, manifest())


# The headers Meson writes into the build tree (configure_file, vcs_tag): the
# image builds scan every file a compile reads, so each needs a rule in the
# real manifest, or every container build stops at its licence scan.
GENERATED_HEADER = re.compile(r"""output\s*:\s*'([\w.]+\.(?:h|asm))'""")


def meson_generated_headers() -> list[str]:
    found = []
    for subdir in ("src", "include"):
        text = (REPO / "core" / subdir / "meson.build").read_text(encoding="utf-8")
        found += [f"{subdir}/{name}" for name in GENERATED_HEADER.findall(text)]
    return sorted(found)


def test_every_meson_generated_header_has_a_licensing_rule() -> None:
    headers = meson_generated_headers()
    assert "src/config.h" in headers and "include/vcs_version.h" in headers
    rules = json.loads(lic.MANIFEST.read_text(encoding="utf-8"))["generated_build_files"]
    for rel in headers:
        lic.generated_rule(rules, rel)


# Each device kernel Meson compiles is embedded as a generated C file, and its
# rule must lead to the one source file Meson names. A kernel outside the
# directory a rule globbed stopped the GPU image builds at their licence scan
# (import_convert under core/src/cuda and core/src/hip).
KERNEL_SOURCE = re.compile(
    r"""^\s*'(\w+)'\s*:\s*\[?\s*(\w+)\s*\+\s*'([\w/]+\.(?:cu|hip))'""", re.MULTILINE
)
KERNEL_DIRS = {
    "src_dir": "core/src/",
    "feature_src_dir": "core/src/feature/",
    "cuda_dir": "core/src/cuda/",
}
KERNEL_OBJECT_SUFFIX = {".cu": ".fatbin.c", ".hip": "_hsaco.c"}


def meson_kernel_objects() -> list[tuple[str, str]]:
    """(generated build input, repository source) of every CUDA and HIP kernel."""
    text = (REPO / "core" / "src" / "meson.build").read_text(encoding="utf-8")
    found = []
    for name, directory, rel in KERNEL_SOURCE.findall(text):
        found.append(
            (f"src/{name}{KERNEL_OBJECT_SUFFIX[Path(rel).suffix]}", KERNEL_DIRS[directory] + rel)
        )
    return sorted(found)


def test_every_meson_kernel_object_resolves_to_the_source_meson_names() -> None:
    kernels = meson_kernel_objects()
    assert {
        "src/psnr_score.fatbin.c",
        "src/import_convert.fatbin.c",
        "src/import_convert_hsaco.c",
    } <= {rel for rel, _ in kernels}
    rules = json.loads(lic.MANIFEST.read_text(encoding="utf-8"))["generated_build_files"]
    for rel, source in kernels:
        assert lic.compiled_source(lic.generated_rule(rules, rel), rel, REPO) == source, rel


def test_a_kernel_outside_the_feature_tree_is_refused_by_a_feature_only_rule() -> None:
    narrow = {"suffix": "_hsaco.c", "compiled_from": "core/src/feature/hip/**/{name}.hip"}
    with pytest.raises(lic.LicensingError, match="identifies 0 files, not 1"):
        lic.compiled_source(narrow, "src/import_convert_hsaco.c", REPO)


# The libraries core/src/meson.build installs, each with its pkg-config file.
PKG_CONFIG_FILEBASE = re.compile(r"""filebase\s*:\s*'([\w.-]+)'""")


def meson_installed_libraries() -> list[str]:
    text = (REPO / "core" / "src" / "meson.build").read_text(encoding="utf-8")
    return sorted(PKG_CONFIG_FILEBASE.findall(text))


def missing_library_entries(data: dict, libraries: list[str]) -> list[str]:
    """Components that record one installed library (`<lib>.so*` or `<lib>.pc`, any
    directory) but not another one in the same place: the build installs them together
    (the compat libvmaf.so.3 on libvmafx.so.1, ADR-2094), so an image or bundle that
    ships one ships both."""
    problems = []
    for kind, record in data["artifacts"].items():
        for component in record["components"]:
            paths = component.get("paths", [])
            for path in paths:
                head, _, name = path.rpartition("/")
                for lib in libraries:
                    for suffix in (".so*", ".pc"):
                        if name != lib + suffix:
                            continue
                        for other in libraries:
                            want = f"{head}/{other}{suffix}" if head else other + suffix
                            if want not in paths:
                                problems.append(f"{kind}/{component['id']}: {path} without {want}")
    return problems


def test_every_installed_library_is_recorded_beside_its_sibling() -> None:
    libraries = meson_installed_libraries()
    assert libraries == ["libvmaf", "libvmafx"]  # the pattern still reads the build
    assert missing_library_entries(lic.load_manifest(), libraries) == []


def test_a_library_recorded_without_its_sibling_is_found() -> None:
    data = {"artifacts": {"img": {"components": [
        {"id": "bin", "paths": ["usr/local/lib/libvmaf.so*", "usr/local/lib/pkgconfig/libvmaf.pc"]},
        {"id": "zip", "paths": ["libvmafx.so*", "libvmaf.so*"]},
    ]}}}  # fmt: skip
    assert missing_library_entries(data, ["libvmaf", "libvmafx"]) == [
        "img/bin: usr/local/lib/libvmaf.so* without usr/local/lib/libvmafx.so*",
        "img/bin: usr/local/lib/pkgconfig/libvmaf.pc without usr/local/lib/pkgconfig/libvmafx.pc",
    ]


def test_scan_build_refuses_a_compiled_file_without_licence(tmp_path: Path, monkeypatch) -> None:
    build, repo = fake_build(tmp_path, monkeypatch)
    write(repo / "core/a.c", "int a;\n")
    with pytest.raises(lic.LicensingError, match="core/a.c has no SPDX header"):
        lic.scan_build(build, repo, manifest())


def test_generated_source_is_identified_by_its_bytes(tmp_path: Path) -> None:
    repo, build = tmp_path / "repo", tmp_path / "build"
    write(repo / "model/a/m.json", "one")
    write(repo / "model/b/m.json", "two")
    write(build / "src/m.json", "two")
    rule = {"pattern": "src/*.json.c", "repo": "model/**/{stem}"}
    assert lic.generated_source(rule, "src/m.json.c", build, repo) == "model/b/m.json"
    (build / "src/m.json").unlink()
    with pytest.raises(lic.LicensingError, match="identifies 2 files"):
        lic.generated_source(rule, "src/m.json.c", build, repo)


FATBIN_RULE = {"pattern": "src/*.fatbin.c", "suffix": ".fatbin.c",
               "compiled_from": "core/src/**/{name}.cu"}  # fmt: skip


def test_a_kernel_object_takes_the_licence_of_the_source_it_was_compiled_from(
    tmp_path: Path, monkeypatch
) -> None:
    build, repo = fake_build(tmp_path, monkeypatch, generated="src/psnr_score.fatbin.c")
    write(repo / "core/src/feature/cuda/integer_psnr/psnr_score.cu",
          f"/* Copyright 2016 Netflix, Inc.\n * {TAG} MIT */\n")  # fmt: skip
    data = manifest()
    data["generated_build_files"].append(FATBIN_RULE)
    entry = next(f for f in lic.scan_build(build, repo, data)["files"]
                 if f["path"] == "src/psnr_score.fatbin.c")  # fmt: skip
    assert entry == {"path": "src/psnr_score.fatbin.c",
                     "from": "core/src/feature/cuda/integer_psnr/psnr_score.cu",
                     "licence": "MIT", "copyright": ["Copyright 2016 Netflix, Inc."]}  # fmt: skip


def test_a_kernel_object_without_exactly_one_source_is_refused(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    with pytest.raises(lic.LicensingError, match="identifies 0 files, not 1"):
        lic.compiled_source(FATBIN_RULE, "src/psnr_score.fatbin.c", repo)
    write(repo / "core/src/feature/cuda/a/psnr_score.cu", "x")
    write(repo / "core/src/feature/cuda/b/psnr_score.cu", "y")
    with pytest.raises(lic.LicensingError, match="identifies 2 files, not 1"):
        lic.compiled_source(FATBIN_RULE, "src/psnr_score.fatbin.c", repo)


VENDOR_ID = "cd" * 20


def vendored_manifest() -> dict:
    """A record whose vendor runtime bundles one copyleft and one permissive library."""
    data = manifest()
    data["source_archives"]["elfutils"] = {"file": "elfutils.tar.bz2", "url": "https://example.invalid/e",
                                           "sha256": "1" * 64, "why": "test"}  # fmt: skip
    data["artifacts"]["kit"]["components"].insert(2, {
        "id": "rocm", "kind": "fixed", "name": "vendor runtime", "licence": "MIT",
        "paths": ["opt/rocm/lib/**"],
        "vendored_libraries": [
            {"pattern": "libsys_elf.so*", "licence": "LGPL-3.0-or-later", "copyleft": True,
             "archives": ["elfutils"], "sources": {VENDOR_ID: ["elfutils"]}},
            {"pattern": "libsys_z.so*", "licence": "Zlib", "copyleft": False},
        ],
    })  # fmt: skip
    return data


def add_vendored(root: Path, build_id: str = VENDOR_ID) -> None:
    lib = root / "opt/rocm/lib/sysdeps"
    write(lib / "libsys_elf.so.1", make_elf(build_id))
    write(lib / "libsys_z.so.1", make_elf("ef" * 20))
    (lib / "libsys_elf.so").symlink_to("libsys_elf.so.1")


def test_a_recorded_vendored_copyleft_library_passes_and_names_its_source(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    add_vendored(Path(args.root))
    data = vendored_manifest()
    assert notices_then_check(args, data) == []
    notices = (Path(args.root) / "licenses" / lic.NOTICES_NAME).read_text()
    assert "Vendored libsys_elf.so*: LGPL-3.0-or-later; source elfutils" in notices
    assert "archive elfutils" in lic.source_list(args, data)


def test_a_vendored_copyleft_library_of_another_build_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    add_vendored(Path(args.root), build_id="99" * 20)
    problems = notices_then_check(args, vendored_manifest())
    assert f"copyleft libsys_elf.so.1 (build ID {'99' * 20}) has no recorded source" in problems


def test_a_recorded_vendored_library_that_is_gone_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    add_vendored(Path(args.root))
    (Path(args.root) / "opt/rocm/lib/sysdeps/libsys_z.so.1").unlink()
    problems = notices_then_check(args, vendored_manifest())
    assert "vendored library libsys_z.so* of component rocm matches no file" in problems


# ----------------------------------------------------------------- sources


def test_sources_list_debian_built_using_and_copyleft_grafts(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    lines = lic.source_list(args, manifest())
    assert lines == ["debian glibc=2.41-1", "debian tar=1.35+dfsg-1", "archive gcc-src"]


def test_fetch_sources_uses_apt_then_records_archives(tmp_path: Path, monkeypatch) -> None:
    listing = write(tmp_path / "list", "debian tar=1.35+dfsg-1\narchive gcc-src\n")
    calls = []

    def run(argv, **kwargs):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, "", "")

    monkeypatch.setattr(lic.subprocess, "run", run)
    monkeypatch.setattr(lic, "download", lambda url, dest, sha: dest.write_text(url))
    args = argparse.Namespace(list=str(listing), out=str(tmp_path / "out"))
    index = lic.fetch_sources(args, manifest())
    assert calls == [["apt-get", "source", "--download-only", "-qq", "tar=1.35+dfsg-1"]]
    assert index[0] == "tar=1.35+dfsg-1  debian/  (archive)"
    assert (
        tmp_path / "out/archives/gcc.src.rpm"
    ).read_text() == "https://example.invalid/gcc.src.rpm"


def test_fetch_debian_falls_back_to_snapshot(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(lic.subprocess, "run",
                        lambda argv, **kw: subprocess.CompletedProcess(argv, 100, "", "gone"))  # fmt: skip
    seen = []
    monkeypatch.setattr(lic, "snapshot_fetch", lambda spec, out: seen.append(spec))
    assert lic.fetch_debian("tar=1.0", tmp_path) == "snapshot" and seen == ["tar=1.0"]


def test_download_refuses_a_wrong_hash(tmp_path: Path, monkeypatch) -> None:
    class Response:
        def __init__(self) -> None:
            self.chunks = [b"payload", b""]

        def read(self, _size: int) -> bytes:
            return self.chunks.pop(0)

        def __enter__(self):
            return self

        def __exit__(self, *_exc) -> None:
            return None

    monkeypatch.setattr(lic.urllib.request, "urlopen", lambda *a, **k: Response())
    with pytest.raises(lic.LicensingError, match="is not the recorded"):
        lic.download("https://example.invalid/x", tmp_path / "x", "0" * 64)
    assert not (tmp_path / "x").exists()


def source_repo(tmp: Path) -> tuple[Path, str]:
    """A local git repository with one commit (the test stand-in for TheRock)."""
    repo = tmp / "upstream"
    write(repo / "patches/numactl/0001.patch", "patch\n")

    def git(*argv: str) -> str:  # never the caller's repository, even under a git hook
        done = subprocess.run(["git", *argv], cwd=repo, check=True, capture_output=True, text=True,
                              env=lic.git_environment())  # fmt: skip
        return done.stdout.strip()

    git("init", "-q")
    git("config", "uploadpack.allowAnySHA1InWant", "true")
    git("add", ".")
    git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "c")
    return repo, git("rev-parse", "HEAD")


def test_a_source_tree_is_fetched_by_its_commit_and_archived(tmp_path: Path) -> None:
    repo, commit = source_repo(tmp_path)
    (tmp_path / "out" / "archives").mkdir(parents=True)
    archive = {"file": "Tree-x.tar.gz", "git": repo.as_uri(), "commit": commit, "why": "test"}
    line = lic.fetch_archive(archive, tmp_path / "out")
    assert f"at commit {commit} (git archive)" in line
    names = subprocess.run(["tar", "-tzf", str(tmp_path / "out/archives/Tree-x.tar.gz")],
                           check=True, capture_output=True, text=True).stdout.split()  # fmt: skip
    assert "Tree-x/patches/numactl/0001.patch" in names
    assert not any(p.name.startswith(".") for p in (tmp_path / "out/archives").iterdir())
    # ADR-1591: gzip at level 9 writes XFL = 2 in byte 8 of its header (RFC 1952).
    assert (tmp_path / "out/archives/Tree-x.tar.gz").read_bytes()[8] == 2


def test_git_commands_ignore_the_callers_repository(monkeypatch) -> None:
    monkeypatch.setenv("GIT_DIR", "/nonexistent/.git")
    monkeypatch.setenv("GIT_INDEX_FILE", "/nonexistent/index")
    env = lic.git_environment()
    assert "GIT_DIR" not in env and "GIT_INDEX_FILE" not in env and "PATH" in env


def test_a_source_tree_of_another_commit_is_refused(tmp_path: Path) -> None:
    repo, commit = source_repo(tmp_path)
    (tmp_path / "out" / "archives").mkdir(parents=True)
    wrong = {"file": "Tree-x.tar.gz", "git": repo.as_uri(), "commit": "0" * 40, "why": "test"}
    with pytest.raises(lic.LicensingError, match="git fetch failed"):
        lic.fetch_archive(wrong, tmp_path / "out")
    short = {**wrong, "commit": commit[:12]}
    with pytest.raises(lic.LicensingError, match="is not a full commit ID"):
        lic.fetch_archive(short, tmp_path / "out")
    assert not (tmp_path / "out/archives/Tree-x.tar.gz").exists()


# ------------------------------------------------- the record and the recipes


def test_the_record_names_texts_and_archives_that_exist() -> None:
    data = lic.load_manifest()
    missing = [text for text in data["spdx_texts"].values() if not (REPO / text).is_file()]
    assert missing == []
    used = {s for rule in data["grafted_libraries"] for s in rule.get("sources", {}).values()}
    assert used <= set(data["source_archives"])
    for name, archive in data["source_archives"].items():  # a static file by hash, a tree by commit
        pinned_file = "url" in archive and re.fullmatch(r"[0-9a-f]{64}", archive.get("sha256", ""))
        pinned_tree = "git" in archive and re.fullmatch(r"[0-9a-f]{40}", archive.get("commit", ""))
        assert pinned_file or pinned_tree, name
    vendored = [rule for r in data["artifacts"].values() for c in r["components"]
                for rule in c.get("vendored_libraries", [])]  # fmt: skip
    assert {a for rule in vendored for a in rule.get("archives", [])} <= set(
        data["source_archives"]
    )
    assert all(
        rule.get("archives") and rule.get("sources") for rule in vendored if rule["copyleft"]
    )
    entries = [
        e for r in data["artifacts"].values() for c in r["components"] for e in c.get("texts", [])
    ]
    assert [e for e in entries if "repo" in e and not (REPO / e["repo"]).is_file()] == []


# The records of the artifacts published before ADR-1513 (ADR-1578) have the
# shape of the record they mirror.
PUBLISHED_RC_MIRRORS = {
    "published-rc-operator-image": "production-operator-image",
    "published-rc-native": "release-native",
    "published-rc-models": "release-models",
}


def test_every_artifact_records_its_interpreter_and_core_components() -> None:
    data = lic.load_manifest()
    for kind, record in data["artifacts"].items():
        shape = PUBLISHED_RC_MIRRORS.get(kind, kind)
        kinds = {c["kind"] for c in record["components"]}
        assert "notices" in kinds, kind
        # every artifact that ships compiled VMAFx files records them from the build
        assert "build" in kinds or shape == "release-models", kind
        # every artifact that ships repository files (models, report code) maps them
        assert "repo" in kinds or shape in {"production-operator-image", "release-native"}, kind
        if "python" in record:  # a python.org interpreter with its packages
            assert record["python"]["version"] in data["cpython_license_rst"], kind
            assert "python-dist" in kinds, kind
        elif not shape.startswith("release-"):  # an image: its distribution's packages
            assert "dpkg" in kinds, kind


def test_the_record_allows_every_licence_the_repo_files_declare() -> None:
    """Every licence a build component allows has a text to ship."""
    data = lic.load_manifest()
    for record in data["artifacts"].values():
        for component in record["components"]:
            missing = set(component.get("licences", [])) - set(data["spdx_texts"])
            assert not missing, (component["id"], missing)


def test_the_image_cannot_be_built_without_the_licence_check() -> None:
    text = (REPO / "docker/Dockerfile.tester").read_text()
    final = text.split("FROM assembled AS final", 1)[1]
    assert "COPY --from=licence-check /out/licence-check.json" in final
    assert "licensing.py check --artifact image" in text
    assert "licensing.py notices --artifact image" in text
    assert "-not -path '*.libs/*'" in text  # vendor libraries ship unmodified
    assert "FROM scratch AS source-export" in text


def assert_gpu_kit_published(kit: str) -> None:
    """The GPU kit is a leg of both matrix jobs, which build its source image and
    attest its SBOM."""
    workflow = (REPO / ".github/workflows/docker-publish-tester.yml").read_text()
    build = workflow.split("  build-gpu:\n", 1)[1].split("\n  publish-gpu:\n", 1)
    assert f"- kit: {kit}\n" in build[0] and f"- kit: {kit}\n" in build[1]
    assert "target: ${{ matrix.kit }}-source-export" in build[0]
    assert "sbom-path: sbom-tester-${{ matrix.kit }}.spdx.json" in build[1]
    assert "environment: tester-publish" in build[1]


def test_the_sycl_image_cannot_be_built_without_its_licence_check() -> None:
    text = (REPO / "docker/Dockerfile.tester").read_text()
    final = text.split("FROM sycl-assembled AS final-sycl", 1)[1].split("\nFROM ", 1)[0]
    assert "COPY --from=sycl-licence-check /out/licence-check.json" in final
    assert "licensing.py check --artifact sycl-image" in text
    assert "licensing.py notices --artifact sycl-image" in text
    assert "FROM scratch AS sycl-source-export" in text
    assert_gpu_kit_published("sycl")


def test_the_cuda_image_cannot_be_built_without_its_licence_check() -> None:
    text = (REPO / "docker/Dockerfile.tester").read_text()
    final = text.split("FROM cuda-assembled AS final-cuda", 1)[1].split("\nFROM ", 1)[0]
    assert "COPY --from=cuda-licence-check /out/licence-check.json" in final
    assert "licensing.py check --artifact cuda-image" in text
    assert "licensing.py notices --artifact cuda-image" in text
    assert "FROM scratch AS cuda-source-export" in text
    runtime = text.split("AS cuda-runtime", 1)[1].split("\nFROM ", 1)[0]
    assert "NVIDIA files in the image" in runtime  # the image ships no NVIDIA library
    assert_gpu_kit_published("cuda")


def test_the_hip_image_cannot_be_built_without_its_licence_check() -> None:
    text = (REPO / "docker/Dockerfile.tester").read_text()
    final = text.split("FROM hip-assembled AS final-hip", 1)[1].split("\nFROM ", 1)[0]
    assert "COPY --from=hip-licence-check /out/licence-check.json" in final
    assert "licensing.py check --artifact hip-image" in text
    assert "licensing.py notices --artifact hip-image" in text
    assert "FROM scratch AS hip-source-export" in text
    assert_gpu_kit_published("hip")


def test_the_hip_record_carries_the_source_of_its_lgpl_libraries() -> None:
    data = lic.load_manifest()
    sysdeps = next(c for c in data["artifacts"]["hip-image"]["components"]
                   if c["id"] == "rocm-sysdeps")  # fmt: skip
    copyleft = [rule for rule in sysdeps["vendored_libraries"] if rule["copyleft"]]
    assert {rule["pattern"] for rule in copyleft} == {"librocm_sysdeps_elf.so*",
                                                      "librocm_sysdeps_numa.so*"}  # fmt: skip
    spec = json.loads((REPO / "tools/rc1-tester/image/hip-runtime.json").read_text())
    shipped = [n for c in spec["components"] if c["id"] == "rocm-sysdeps" for n in c["names"]]
    for name in shipped:  # every bundled library the image ships has a vendored rule
        assert any(lic.fnmatch.fnmatchcase(name.rstrip("*") or name, r["pattern"])
                   for r in sysdeps["vendored_libraries"]), name  # fmt: skip


def test_the_cuda_record_carries_the_nvidia_terms() -> None:
    record = lic.load_manifest()["artifacts"]["cuda-image"]
    nvidia = next(c for c in record["components"] if c["id"] == "nvidia-cuda-device-code")
    assert "paths" not in nvidia  # no NVIDIA file ships; the code is inside VMAFx binaries
    assert nvidia["texts"][0]["artifact"] == "opt/vmafx/licenses/nvidia/CUDA-EULA.txt"
    assert any("EUPL-1.2 covers only the VMAFx files" in note for note in nvidia["notes"])


def test_the_bundle_script_checks_before_it_packs() -> None:
    text = (REPO / "scripts/ci/build-macos-tester-bundle.sh").read_text()
    assert text.index("licensing check --artifact macos") < text.index('step "pack"')
    assert text.index("licensing notices --artifact macos") < text.index('step "the bundle')
    assert "PBS_FULL_SHA256" in text


def test_the_windows_build_checks_before_it_packs() -> None:
    text = (REPO / "scripts/ci/build-windows-tester-bundle.py").read_text()
    body = text.split("def seal(", 1)[1].split("\ndef ", 1)[0]
    assert body.index('licensing("notices", "--artifact", kind') < body.index("run_own_report(")
    assert body.index("check-windows-bundle-imports.py") < body.index("pack(bundle")
    assert body.index('licensing("check", "--artifact", kind') < body.index("pack(bundle")
    assert "kind, suffix = KITS[gpu]" in text.split("def build_all(", 1)[1]
    kits = text.split("KITS = {", 1)[1].split("}", 1)[0]
    artifacts = lic.load_manifest()["artifacts"]
    for kind in ("windows-zip", "windows-cuda-zip", "windows-sycl-zip"):
        assert f'"{kind}"' in kits and kind in artifacts, kind


@pytest.mark.parametrize(("workflow", "needle"), [
    ("docker-publish-tester.yml", "sbom-path: ${{ runner.temp }}/sbom/tester-sbom-arm64/sbom.spdx.json"),
    ("docker-publish-tester.yml", "target: source-export"),
    ("macos-tester-bundle.yml", "sbom-path: ${{ steps.sbom.outputs.path }}"),
    ("macos-tester-bundle.yml", "PBS_FULL_SHA256:"),
    ("windows-tester-bundle.yml", "sbom-path: ${{ steps.assets.outputs.sbom_x64 }}"),
    ("windows-tester-bundle.yml", "sbom-path: ${{ steps.assets.outputs.sbom_arm64 }}"),
    ("windows-tester-bundle.yml", "PBS_ARM64_FULL_SHA256:"),
    ("windows-tester-bundle.yml", "sbom-path: ${{ steps.assets.outputs.sbom_x64_cuda }}"),
    ("windows-tester-bundle.yml", "sbom-path: ${{ steps.assets.outputs.sbom_x64_sycl }}"),
])  # fmt: skip
def test_both_workflows_attest_an_sbom(workflow: str, needle: str) -> None:
    assert needle in (REPO / ".github/workflows" / workflow).read_text()


# ------------------------------------------------------------ the Windows zip


VS_TERMS = "visual-studio-2026-license-terms.txt"


def windows_tree(tmp: Path) -> argparse.Namespace:
    """A tree shaped like the Windows zip, checked against the real windows-zip record."""
    root = tmp / "zip"
    for rel in ("build/tools/vmaf.exe", "tests/test_cpu.exe", "runtime/python.exe",
                "runtime/python313.dll", "runtime/vcruntime140.dll", "runtime/vcruntime140_1.dll",
                "runtime/DLLs/_ctypes.pyd", "runtime/Lib/os.py",
                "runtime/LICENSE.txt", "python/test/resource/yuv/src01_hrc00_576x324.yuv",
                "reference/x86_64-default.json", "image/build-info.json", "image/unit-tests.json",
                "image/package-arch.txt", "image/msvc-redist.json"):  # fmt: skip
        write(root / rel, "x")
    for artifact, repo in (("tester/vmaf-tester-report", "tools/rc1-tester/vmaf-tester-report"),
                           ("tester/src/vmaf_rc1_tester/hw_report.py",
                            "tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py"),
                           ("image/fixtures.json", "tools/rc1-tester/image/fixtures.json"),
                           ("run.cmd", "tools/rc1-tester/image/windows/run.cmd"),
                           ("README.txt", "tools/rc1-tester/image/windows/README.txt")):  # fmt: skip
        write(root / artifact, (REPO / repo).read_bytes())
    texts = tmp / "texts"
    write(texts / "cpython-license.rst", "Doc/license.rst\n")
    write(texts / "python-build-standalone/LICENSE.openssl-3.txt", "Apache-2.0\n")
    write(texts / VS_TERMS, "MICROSOFT SOFTWARE LICENSE TERMS\nLast Updated: October 1, 2025.\n")
    scan_path = write(tmp / "scan.json", json.dumps(scan(("EUPL-1.2", "BSD-2-Clause-Patent"))))
    return argparse.Namespace(artifact="windows-zip", root=str(root), repo=str(REPO),
                              build_scan=str(scan_path), texts=str(texts), source_commit="c0ffee",
                              tag="v1.0.0-test", python_version="3.13.16", receipt=None)  # fmt: skip


def windows_cuda_tree(tmp: Path) -> argparse.Namespace:
    """The Windows zip's tree with what the CUDA zip adds (ADR-1516)."""
    args = windows_tree(tmp)
    args.artifact = "windows-cuda-zip"
    root = Path(args.root)
    for rel in ("tests/test_cuda_psnr_parity.exe", "image/gpu-tests.json", "image/gpu-twins.json",
                "image/cuda-targets.json"):  # fmt: skip
        write(root / rel, "x")
    write(
        root / "image/cuda-rows.json", (REPO / "tools/rc1-tester/image/cuda-rows.json").read_bytes()
    )
    write(root / "tester/gate/scripts/ci/cross_backend_parity_gate.py",
          (REPO / "scripts/ci/cross_backend_parity_gate.py").read_bytes())  # fmt: skip
    write(
        root / "licenses/nvidia/CUDA-EULA.txt", "Last updated: January 26, 2026\nlibdevice.10.bc\n"
    )
    write(root / "licenses/nv-codec-headers/NOTICE.txt", "Permission is hereby granted\n")
    return args


def test_the_windows_cuda_zip_record_passes_and_refuses_an_nvidia_library(tmp_path: Path) -> None:
    args = windows_cuda_tree(tmp_path)
    data = lic.load_manifest()
    assert notices_then_check(args, data) == []
    root = Path(args.root)
    notices = (root / "licenses/THIRD_PARTY_NOTICES.txt").read_text()
    assert "[component nvidia-cuda-device-code]" in notices and "nvcuda.dll" in notices
    write(root / "tests/cudart64_13.dll", "an NVIDIA runtime the zip must not carry")
    (root / "licenses/nvidia/CUDA-EULA.txt").unlink()
    problems = lic.run_check(args, data)
    assert "no recorded licence: tests/cudart64_13.dll" in problems
    assert "licence text /licenses/nvidia/CUDA-EULA.txt is missing" in problems


def windows_sycl_tree(tmp: Path) -> argparse.Namespace:
    """The Windows zip's tree with what the SYCL zip adds (ADR-1566): the runtime DLLs,
    Intel's runtime and the Level Zero loader beside each program, and their texts."""
    args = windows_tree(tmp)
    args.artifact = "windows-sycl-zip"
    root = Path(args.root)
    for directory in ("build/tools", "tests"):
        for name in ("sycl8.dll", "ur_loader.dll", "ur_adapter_level_zero.dll", "libmmd.dll",
                     "umf.dll", "libhwloc-15.dll", "ze_loader.dll", "vcruntime140.dll",
                     "msvcp140.dll"):  # fmt: skip
            write(root / directory / name, "MZ")
    for rel in ("tests/test_sycl_psnr_parity.exe", "image/gpu-tests.json", "image/gpu-twins.json",
                "image/gpu-runtime.json"):  # fmt: skip
        write(root / rel, "x")
    write(
        root / "image/sycl-rows.json", (REPO / "tools/rc1-tester/image/sycl-rows.json").read_bytes()
    )
    write(
        root / "image/scratch_ratchet.txt",
        (REPO / "core/src/sycl/scratch_ratchet.txt").read_bytes(),
    )
    write(root / "tester/gate/scripts/ci/cross_backend_parity_gate.py",
          (REPO / "scripts/ci/cross_backend_parity_gate.py").read_bytes())  # fmt: skip
    for component, texts in (("intel-oneapi-dpcpp-runtime", ("LICENSE.rtf", "third-party-programs.txt", "credist.txt")),
                             ("intel-oneapi-umf", ("LICENSE.TXT", "third-party-programs.txt")),
                             ("intel-oneapi-tcm-hwloc", ("license.txt", "third-party-programs.txt"))):  # fmt: skip
        for text in texts:
            write(root / "licenses/intel" / component / text, "Intel text\n")
    write(Path(args.texts) / "level-zero-1.34.0-LICENSE.txt", "MIT License\n")
    return args


def test_the_windows_sycl_zip_record_passes_and_refuses_planted_defects(tmp_path: Path) -> None:
    args = windows_sycl_tree(tmp_path)
    data = lic.load_manifest()
    assert notices_then_check(args, data) == []
    root = Path(args.root)
    notices = (root / "licenses/THIRD_PARTY_NOTICES.txt").read_text()
    for component in ("intel-oneapi-dpcpp-runtime", "level-zero-loader", "microsoft-vc-runtime-programs",
                      "microsoft-linked-runtime"):  # fmt: skip
        assert f"[component {component}]" in notices, component
    write(root / "tests/ur_adapter_opencl.dll", "an adapter the zip must not carry")
    write(root / "build/tools/sycl-jit.dll", "x")
    (root / "licenses/intel/intel-oneapi-dpcpp-runtime/LICENSE.rtf").unlink()
    problems = lic.run_check(args, data)
    assert "no recorded licence: tests/ur_adapter_opencl.dll" in problems
    assert "no recorded licence: build/tools/sycl-jit.dll" in problems
    assert any("intel-oneapi-dpcpp-runtime/LICENSE.rtf is missing" in p for p in problems), problems


def test_the_windows_zip_record_passes_a_recorded_tree(tmp_path: Path) -> None:
    args = windows_tree(tmp_path)
    data = lic.load_manifest()
    assert notices_then_check(args, data) == []
    notices = (Path(args.root) / "licenses/THIRD_PARTY_NOTICES.txt").read_text()
    for component in ("microsoft-static-runtime", "microsoft-vc-runtime", "cpython"):
        assert f"[component {component}]" in notices
    assert "not covered by EUPL-1.2" in notices
    assert f"texts/{VS_TERMS}" in notices and "October 1, 2025" in notices
    assert (Path(args.root) / "licenses/texts" / VS_TERMS).is_file()


def test_both_windows_zips_carry_the_visual_studio_terms_from_one_record() -> None:
    data = lic.expand_shared(lic.load_manifest())
    fetched = data["fetched_texts"][VS_TERMS]
    assert fetched["extract"] == "docx-text" and len(fetched["sha256"]) == 64
    for kind in ("windows-zip", "windows-cuda-zip"):
        record = lic.artifact_record(data, kind)
        for component in record["components"]:
            if component["id"].startswith("microsoft-"):
                assert [t["fetched"] for t in component["texts"]] == [VS_TERMS], kind


def test_the_windows_zip_gate_refuses_planted_defects(tmp_path: Path) -> None:
    args = windows_tree(tmp_path)
    data = lic.load_manifest()
    lic.write_notices(args, data)
    root = Path(args.root)
    write(root / "tests/vcruntime140.dll", "a Microsoft DLL where only VMAFx programs belong")
    write(root / "runtime-extra.dll", "x")
    (root / "runtime/LICENSE.txt").unlink()
    (root / "licenses/texts" / VS_TERMS).unlink()
    problems = lic.run_check(args, data)
    assert "no recorded licence: tests/vcruntime140.dll" in problems
    assert "no recorded licence: runtime-extra.dll" in problems
    assert "licence text /runtime/LICENSE.txt is missing" in problems
    assert any(VS_TERMS in problem and "missing" in problem for problem in problems), problems


# ------------------------------------------------- vendor packages, fetched texts


def foreign_record() -> dict:
    data = record()
    data["components"].insert(4, {
        "id": "vendor", "kind": "dpkg-foreign", "name": "vendor GPU stack", "packages": ["libvendor1"],
        "licence": "MIT", "source": "https://example.invalid/vendor",
        "texts": [{"fetched": "vendor-LICENSE.txt", "name": "vendor-LICENSE.txt", "label": "vendor"}],
    })  # fmt: skip
    return data


def add_vendor_package(root: Path) -> None:
    status = root / "var/lib/dpkg/status"
    status.write_text(status.read_text() + "Package: libvendor1\nStatus: install ok installed\n"
                      "Version: 26.35-0\n\n")  # fmt: skip
    write(root / "var/lib/dpkg/info/libvendor1.list", "/.\n/usr\n/usr/lib/libvendor.so.1\n")
    write(root / "usr/lib/libvendor.so.1", "vendor")


def vendor_manifest() -> dict:
    data = manifest()
    data["artifacts"]["kit"] = foreign_record()
    data["fetched_texts"] = {"vendor-LICENSE.txt": {"url": "https://example.invalid/LICENSE",
                                                    "sha256": "0" * 64, "why": "test"}}  # fmt: skip
    return data


def test_a_recorded_vendor_package_needs_no_copyright_file_and_no_debian_source(
    tmp_path: Path,
) -> None:
    args = setup_tree(tmp_path)
    add_vendor_package(Path(args.root))
    write(Path(args.texts) / "vendor-LICENSE.txt", "MIT text\n")
    data = vendor_manifest()
    assert notices_then_check(args, data) == []
    notices = (Path(args.root) / "licenses/THIRD_PARTY_NOTICES.txt").read_text()
    assert "[component vendor]" in notices and "libvendor1 26.35-0" in notices
    assert not any("libvendor1" in line for line in lic.source_list(args, data))


def test_an_unrecorded_vendor_package_still_needs_its_copyright_file(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    add_vendor_package(Path(args.root))
    problems = notices_then_check(args)  # the record has no dpkg-foreign component
    assert "package libvendor1 has no /usr/share/doc/*/copyright" in problems
    assert "debian libvendor1=26.35-0" in lic.source_list(args, manifest())


def test_a_recorded_vendor_package_that_is_not_installed_fails(tmp_path: Path) -> None:
    args = setup_tree(tmp_path)
    write(Path(args.texts) / "vendor-LICENSE.txt", "MIT text\n")
    assert "recorded vendor package libvendor1 is not installed" in notices_then_check(
        args, vendor_manifest()
    )


def test_fetch_texts_downloads_the_recorded_texts_with_their_hash(
    tmp_path: Path, monkeypatch
) -> None:
    calls = []
    monkeypatch.setattr(lic, "download", lambda url, dest, sha: calls.append((url, dest.name, sha)))
    args = argparse.Namespace(artifact="kit", python_version="none", out=str(tmp_path / "t"))
    data = vendor_manifest()
    del data["artifacts"]["kit"]["python"]
    lic.fetch_texts(args, data)
    assert calls == [("https://example.invalid/LICENSE", "vendor-LICENSE.txt", "0" * 64)]
    del data["fetched_texts"]
    with pytest.raises(lic.LicensingError, match="no entry in fetched_texts"):
        lic.fetch_texts(args, data)


def make_docx(path: Path, document_xml: str) -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("[Content_Types].xml", "<Types/>")
        archive.writestr("word/document.xml", document_xml)
    return path


DOCX_BODY = (
    '<w:document xmlns:w="w"><w:body>'
    '<w:p><w:pPr><w:tabs><w:tab w:val="left" w:pos="720"/></w:tabs></w:pPr>'
    "<w:r><w:t>TERMS</w:t></w:r></w:p>"
    '<w:p><w:r><w:t xml:space="preserve">A &amp; B </w:t></w:r><w:r><w:tab/><w:t>C</w:t></w:r></w:p>'
    "<w:p/>"
    "</w:body></w:document>"
)


def test_a_docx_text_is_extracted_paragraph_by_paragraph(tmp_path: Path, monkeypatch) -> None:
    source = make_docx(tmp_path / "terms.docx", DOCX_BODY)
    monkeypatch.setattr(lic, "download", lambda url, dest, sha: shutil.copyfile(source, dest))
    out = tmp_path / "out"
    out.mkdir()
    spec = {"url": "https://example.invalid/terms.docx", "sha256": "1" * 64, "extract": "docx-text"}
    lic.fetch_text("terms.txt", spec, out)
    text = (out / "terms.txt").read_text()
    assert text.startswith("Text of https://example.invalid/terms.docx\n")
    assert text.endswith("TERMS\nA & B \tC\n\n")  # tab stops of a paragraph are no tabs
    assert sorted(p.name for p in out.iterdir()) == ["terms.txt"]


def test_a_docx_extract_refuses_what_it_cannot_read(tmp_path: Path, monkeypatch) -> None:
    out = tmp_path / "out"
    out.mkdir()
    spec = {"url": "https://example.invalid/x", "sha256": "1" * 64, "extract": "docx-text"}
    garbage = write(tmp_path / "garbage.docx", "not a zip")
    empty = make_docx(tmp_path / "empty.docx", "<w:document><w:body><w:p/></w:body></w:document>")
    for source, message in ((garbage, "not a readable .docx"), (empty, "has no text")):
        monkeypatch.setattr(
            lic, "download", lambda url, dest, sha, s=source: shutil.copyfile(s, dest)
        )
        with pytest.raises(lic.LicensingError, match=message):
            lic.fetch_text("x.txt", spec, out)
    with pytest.raises(lic.LicensingError, match="unknown extract 'pdf-text'"):
        lic.fetch_text("x.txt", {**spec, "extract": "pdf-text"}, out)
