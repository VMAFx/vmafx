# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Go programs and copied Debian libraries in the licence gate (ADR-1513): the
build information read from the binary, the module licence texts it requires,
the corresponding source a copyleft module brings, and libraries copied out of
their packages."""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import re
import struct
import zipfile
from pathlib import Path

import pytest

_IMAGE = Path(__file__).resolve().parents[1] / "image"
_spec = importlib.util.spec_from_file_location("licensing", _IMAGE / "licensing.py")
lic = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lic)

REPO = Path(__file__).resolve().parents[3]
SENTINEL = "0" * 16
MIT = "Permission is hereby granted, " + "free of charge, to any person obtaining a copy\n"
MPL = "Mozilla Public License Version 2.0\n==================================\n"
LGPL3 = "GNU LESSER GENERAL PUBLIC LICENSE\n                       Version 3, 29 June 2007\n"


def write(path: Path, text: str | bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text) if isinstance(text, bytes) else path.write_text(text)
    return path


def varint(value: int) -> bytes:
    out = bytearray()
    for _ in range(10):
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)
    raise AssertionError("value too large")


def go_elf(modinfo: str, version: str = "go1.27.1") -> bytes:
    """A 64-bit ELF with a `.go.buildinfo` section in the inline-string form."""
    payload = lic.GO_MAGIC + bytes([8, 2]) + bytes(16)
    for text in (version, SENTINEL + modinfo + SENTINEL):
        payload += varint(len(text.encode())) + text.encode()
    names = b"\0.shstrtab\0.go.buildinfo\0"
    shoff = 64
    data_off = shoff + 3 * 64
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HHH", header, 0x3A, 64, 3, 1)
    strtab = bytearray(64)
    struct.pack_into("<I", strtab, 0, 1)
    struct.pack_into("<QQ", strtab, 0x18, data_off, len(names))
    info = bytearray(64)
    struct.pack_into("<I", info, 0, 11)
    struct.pack_into("<QQ", info, 0x18, data_off + len(names), len(payload))
    return bytes(header) + bytes(64) + bytes(strtab) + bytes(info) + names + payload


MODINFO = (
    "path\tgithub.com/VMAFx/vmafx/cmd/x\n"
    "mod\tgithub.com/VMAFx/vmafx\t(devel)\t\n"
    "dep\texample.com/mit\tv1.0.0\th1:mit=\n"
    "dep\texample.com/old\tv0.1.0\th1:old=\n"
    "=>\texample.com/fork\tv0.2.0\th1:fork=\n"
    "dep\texample.com/mpl\tv2.0.0\th1:mpl=\n"
)


def manifest() -> dict:
    return {"go_own_modules": ["github.com/VMAFx/vmafx"], "go_module_licences": {}}


# ------------------------------------------------------------ build info


def test_build_info_lists_dependencies_with_replacements(tmp_path: Path) -> None:
    binary = write(tmp_path / "x", go_elf(MODINFO))
    info = lic.go_buildinfo(binary)
    assert info["go"] == "go1.27.1"
    assert info["main"]["path"] == "github.com/VMAFx/vmafx"
    assert [m["path"] for m in info["deps"]] == [
        "example.com/mit",
        "example.com/fork",
        "example.com/mpl",
    ]
    keys = [lic.module_key(m) for m in lic.go_modules(binary, {"github.com/VMAFx/vmafx"})]
    assert keys == ["example.com/mit@v1.0.0", "example.com/fork@v0.2.0", "example.com/mpl@v2.0.0"]


def test_a_vendor_program_counts_its_main_module(tmp_path: Path) -> None:
    binary = write(tmp_path / "rclone", go_elf("mod\tgithub.com/rclone/rclone\tv1.75.1\th1:r=\n"))
    assert [m["path"] for m in lic.go_modules(binary, set())] == ["github.com/rclone/rclone"]


def test_a_file_without_build_info_is_refused(tmp_path: Path) -> None:
    assert lic.go_buildinfo(write(tmp_path / "c", b"\x7fELF\x02\x01" + bytes(58))) is None
    with pytest.raises(lic.LicensingError, match="no Go build information"):
        lic.go_modules(write(tmp_path / "t", "text"), set())


@pytest.mark.parametrize(("text", "expected"), [
    (MIT, "MIT"), (MPL, "MPL-2.0"), (LGPL3, "LGPL-3.0"),
    ("Apache License\n                           Version 2.0, January 2004\n", "Apache-2.0"),
    ("Redistribution and use in source " + "and binary forms ... endorse or promote products", "BSD-3-Clause"),
    ("Redistribution and use in source " + "and binary forms, with or without", "BSD-2-Clause"),
    ("Permission to use, copy, modify, and distribute this software for any purpose", "ISC"),
    ("EUROPEAN UNION PUBLIC LICENCE v. 1.2\n... Mozilla Public License ...", "EUPL-1.2"),
    ("DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\nVersion 2, December 2004", "WTFPL"),
    ("some private terms", "UNKNOWN"),
])  # fmt: skip
def test_licence_texts_are_classified(text: str, expected: str) -> None:
    assert lic.classify_licence_text(text) == expected


# ------------------------------------------------------------- the gate


def go_tree(tmp_path: Path) -> tuple[object, dict]:
    root = tmp_path / "root"
    write(root / "usr/local/bin/x", go_elf(MODINFO))
    texts = root / "licenses/go"
    write(texts / "example.com/mit@v1.0.0/LICENSE", MIT)
    write(texts / "example.com/fork@v0.2.0/LICENSE.txt", MIT)
    write(texts / "example.com/mpl@v2.0.0/LICENSE", MPL)
    record = {"licence_root": "licenses", "components": [
        {"id": "go", "kind": "go-binary", "name": "Go modules", "paths": ["usr/local/bin/x"]}]}  # fmt: skip
    return lic.Context(root, tmp_path, record), manifest()


def test_every_module_with_a_recognised_text_passes(tmp_path: Path) -> None:
    ctx, data = go_tree(tmp_path)
    assert lic.go_problems(ctx, data) == []


def test_a_module_without_its_text_fails(tmp_path: Path) -> None:
    ctx, data = go_tree(tmp_path)
    (ctx.root / "licenses/go/example.com/mpl@v2.0.0/LICENSE").unlink()
    assert lic.go_problems(ctx, data) == [
        "Go module example.com/mpl@v2.0.0 has no licence text in the licence directory"
    ]


def test_an_unrecognised_licence_needs_a_recorded_one(tmp_path: Path) -> None:
    ctx, data = go_tree(tmp_path)
    write(ctx.root / "licenses/go/example.com/mit@v1.0.0/LICENSE", "some private terms")
    assert "licence not recognised" in lic.go_problems(ctx, data)[0]
    data["go_module_licences"]["example.com/mit"] = {"licence": "MIT"}
    assert lic.go_problems(ctx, data) == []


def test_a_recorded_program_missing_from_the_artifact_fails(tmp_path: Path) -> None:
    ctx, data = go_tree(tmp_path)
    (ctx.root / "usr/local/bin/x").unlink()
    assert lic.go_problems(ctx, data) == [
        "recorded Go program usr/local/bin/x is not in the artifact"
    ]


def test_copyleft_modules_bring_their_source(tmp_path: Path) -> None:
    ctx, data = go_tree(tmp_path)
    assert lic.go_source_lines(ctx, data) == ["gomod example.com/mpl@v2.0.0 h1:mpl="]
    write(ctx.root / "licenses/go/example.com/fork@v0.2.0/LICENSE.txt", LGPL3)
    lines = lic.go_source_lines(ctx, data)  # an LGPL module: every module of the program
    assert lines == sorted(f"gomod {k}" for k in
                           ("example.com/mit@v1.0.0 h1:mit=", "example.com/fork@v0.2.0 h1:fork=",
                            "example.com/mpl@v2.0.0 h1:mpl="))  # fmt: skip


def test_module_zip_hash_is_go_dirhash_h1(tmp_path: Path) -> None:
    archive = tmp_path / "m.zip"
    with zipfile.ZipFile(archive, "w") as out:
        out.writestr("example.com/m@v1.0.0/go.mod", "module example.com/m\n")
        out.writestr("example.com/m@v1.0.0/a.go", "package m\n")
    lines = "".join(f"{hashlib.sha256(body).hexdigest()}  {name}\n" for name, body in sorted(
        [("example.com/m@v1.0.0/a.go", b"package m\n"), ("example.com/m@v1.0.0/go.mod", b"module example.com/m\n")]))  # fmt: skip
    assert (
        lic.go_zip_hash(archive)
        == "h1:" + base64.b64encode(hashlib.sha256(lines.encode()).digest()).decode()
    )
    assert lic.module_escape("github.com/BurntSushi/toml") == "github.com/!burnt!sushi/toml"


def test_licence_file_names_of_a_module(tmp_path: Path) -> None:
    for name in (
        "LICENSE",
        "LICENSE.md",
        "LICENSE_BSD-2.txt",
        "COPYING",
        "NOTICE.txt",
        "UNLICENSE",
    ):
        write(tmp_path / "src" / name, MIT)
    write(tmp_path / "src/README.md", "readme")
    copied = lic.copy_module_texts(tmp_path / "src", tmp_path / "out")
    assert copied == [
        "COPYING",
        "LICENSE",
        "LICENSE.md",
        "LICENSE_BSD-2.txt",
        "NOTICE.txt",
        "UNLICENSE",
    ]


# ------------------------------------------------- copied Debian libraries


def copied_tree(tmp_path: Path) -> object:
    root = tmp_path / "root"
    write(root / "usr/local/lib/libx264.so.164", "elf")
    write(root / "share/copied/packages.list",
          "usr/local/lib/libx264.so.164 libx264-164 2:0.164-2+b1 x264=2:0.164-2\n")  # fmt: skip
    write(root / "share/copied/libx264-164/copyright", "GPL-2+\n")
    record = {"licence_root": "licenses", "components": [
        {"id": "copied", "kind": "dpkg-copied", "name": "copied", "list": "share/copied/packages.list",
         "root": "share/copied"}]}  # fmt: skip
    return lic.Context(root, tmp_path, record)


def test_a_copied_library_is_claimed_and_brings_its_debian_source(tmp_path: Path) -> None:
    ctx = copied_tree(tmp_path)
    component = ctx.record["components"][0]
    assert lic.copied_problems(ctx) == []
    assert lic.copied_claims(component, ctx, "usr/local/lib/libx264.so.164")
    assert lic.copied_claims(component, ctx, "share/copied/libx264-164/copyright")
    assert not lic.copied_claims(component, ctx, "usr/local/lib/libother.so.1")
    assert lic.copied_specs(ctx) == {"x264=2:0.164-2"}


def test_a_copied_library_without_its_copyright_file_fails(tmp_path: Path) -> None:
    ctx = copied_tree(tmp_path)
    (ctx.root / "share/copied/libx264-164/copyright").unlink()
    assert lic.copied_problems(ctx) == [
        "copied library usr/local/lib/libx264.so.164: package libx264-164 has no copyright file"
    ]


def test_a_listed_library_missing_from_the_artifact_fails(tmp_path: Path) -> None:
    ctx = copied_tree(tmp_path)
    (ctx.root / "usr/local/lib/libx264.so.164").unlink()
    assert "listed but not in the artifact" in lic.copied_problems(ctx)[0]


# ------------------------------------------------------------- contracts


@pytest.mark.parametrize(("dockerfile", "target", "kind", "source"), [
    ("docker/Dockerfile.operator", "operator", "production-operator-image", "source-export"),
    ("Dockerfile.go-server", "go-server", "production-go-server-image", "source-export"),
    ("docker/Dockerfile.controller", "controller", "production-controller-image", "source-export"),
    ("docker/Dockerfile.node", "node-cpu", "production-node-image", "node-source-export"),
])  # fmt: skip
def test_the_go_images_collect_module_licences_and_publish_source(
    dockerfile: str, target: str, kind: str, source: str
) -> None:
    text = (REPO / dockerfile).read_text()
    assert "licensing.py go-licences" in text and "licensing.py scan-go" in text
    assert f"licensing.py check --artifact {kind}" in text
    assert f"FROM scratch AS {source}\n" in text
    workflow = (REPO / ".github/workflows/docker-publish-operator-node.yml").read_text()
    assert f"target: {source}\n" in workflow


def test_a_notices_stage_with_an_empty_texts_folder_has_no_fetched_text() -> None:
    manifest = lic.load_manifest()
    empty = re.compile(r"mkdir /texts \\\n\s*&& python3 \S+ notices --artifact (\S+)")
    found = [
        kind
        for path in [REPO / "Dockerfile.go-server", *(REPO / "docker").glob("Dockerfile.*")]
        for kind in empty.findall(path.read_text())
    ]
    assert found == ["production-operator-image"]
    assert not lic.recorded_fetches(lic.artifact_record(manifest, found[0]), manifest)


def test_the_node_ffmpeg_is_redistributable_and_its_source_exact() -> None:
    text = (REPO / "docker/Dockerfile.node").read_text()
    assert "--enable-nonfree" not in re.sub(r"(?m)^#.*$", "", text)
    assert "git archive --format=tar.gz" in text and "CONFIGURE.txt" in text
    assert "record-copied-debian-libs" in text
    assert re.search(r"(?m)^FROM \$\{RELEASE_GO_BASE\} AS rclone-bin$", text)
