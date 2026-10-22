#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Licence record, notices and gate of the published tester artifacts (ADR-1503).

Build-time only; never runs on a tester's machine. Standard library, Python 3.11+.

scan-build   --build DIR --repo DIR --out FILE
             licence and copyright of every repository file the build compiled
             (from `ninja -t deps`), and of the generated files it compiled
fetch-texts  --artifact KIND --python-version V --out DIR
             download the recorded licence texts that are not in the repository
notices      --artifact KIND --root DIR --repo DIR --build-scan FILE --texts DIR
             --source-commit SHA --tag TAG
             write <licence root>/THIRD_PARTY_NOTICES.txt and the licence texts
check        --artifact KIND --root DIR --repo DIR --build-scan FILE --python-version V
             exit 1 when a file of the artifact has no recorded licence (ADR-1503 rule 3)
sources      --artifact KIND --root DIR --repo DIR --out FILE
             list the source packages the artifact's copyleft object code needs
fetch-sources --list FILE --out DIR
             download those source packages (apt-get source, snapshot.debian.org,
             recorded archives by SHA-256, recorded source trees by git commit,
             Go module zips from proxy.golang.org by the binary's h1 hash)
scan-go      --package PKG [--package PKG ...] --repo DIR --out FILE [--merge FILE]
             licence and copyright of every own Go file the programs compile
             (`go list -deps`), merged into a scan-build output when given
go-licences  --binary FILE [--binary FILE ...] --out DIR
             copy the licence and notice files of every module the Go programs
             link out of the module cache into DIR/<module>@<version>/

The record is licensing.json next to this file.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import html
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

import tomllib

MANIFEST = Path(__file__).with_name("licensing.json")
NOTICES_NAME = "THIRD_PARTY_NOTICES.txt"
HEADER_BYTES = 6000
MAX_FILES = 400_000
MAX_DOWNLOAD = 512 * 1024 * 1024
TIMEOUT = 300
# A host that answers 429 or a 5xx gateway error is asked again, at most this often,
# after its Retry-After (capped) or a doubling wait; any other error fails at once.
HTTP_ATTEMPTS = 5
HTTP_RETRY_STATUS = frozenset({429, 502, 503, 504})
HTTP_MAX_WAIT = 60
# Spelled in two parts so that REUSE tooling does not read these patterns as tags.
SPDX_TAG = "SPDX-" + "License-Identifier:"
COPYRIGHT_TAG = "SPDX-" + "FileCopyrightText:"
SPDX_LINE = re.compile(re.escape(SPDX_TAG) + r"\s*(?P<expr>[^\n]*)")
COPYRIGHT_LINE = re.compile(
    r"(?:Copyright\b|" + re.escape(COPYRIGHT_TAG) + r")[^\n]*", re.IGNORECASE
)
SPDX_OPERATORS = {"AND", "OR", "WITH"}
LICENCE_FILE = re.compile(r"(?i)^(licen[cs]e|copying|notice|authors)")


class LicensingError(RuntimeError):
    """The record, the artifact or an input is inconsistent; main() prints it, exit 1."""


# --------------------------------------------------------------------------- record


def load_manifest(path: Path = MANIFEST) -> dict:
    return expand_shared(json.loads(path.read_text(encoding="utf-8")))


def shared_component(artifacts: dict, entry: dict, rewrite: dict[str, str]) -> dict:
    """The component `entry` names ({"from": <artifact>, "id": <component>}), with the
    record's `rewrite` path prefixes applied to every string in it and the entry's
    other keys replacing the shared ones (ADR-1517)."""
    source = next(
        (c for c in artifacts.get(entry["from"], {}).get("components", [])
         if c.get("id") == entry["id"]),
        None,
    )  # fmt: skip
    if source is None or "from" in source:
        raise LicensingError(
            f"{entry['from']} has no component {entry['id']!r} of its own in {MANIFEST.name}"
        )
    text = json.dumps(source)
    for old, new in rewrite.items():
        text = text.replace(old, new)
    overrides = {key: value for key, value in entry.items() if key != "from"}
    return {**json.loads(text), **overrides}


def expand_shared(manifest: dict) -> dict:
    """Replace every shared-component entry by the component it names, so one
    component (a vendor runtime, its texts and notes) has one definition however
    many artifacts ship it; a shared component never names another one."""
    artifacts = manifest.get("artifacts", {})
    expanded = {}
    for kind, record in artifacts.items():
        rewrite = record.get("rewrite", {})
        components = [
            shared_component(artifacts, c, rewrite) if "from" in c else c
            for c in record.get("components", [])
        ]
        expanded[kind] = {**record, "components": components}
    manifest["artifacts"] = expanded
    return manifest


def artifact_record(manifest: dict, kind: str) -> dict:
    try:
        return manifest["artifacts"][kind]
    except KeyError as error:
        raise LicensingError(f"no artifact kind {kind!r} in {MANIFEST.name}") from error


def spdx_ids(expression: str) -> set[str]:
    """Licence and exception identifiers of an SPDX expression."""
    tokens = re.split(r"[\s()]+", expression.strip())
    return {token for token in tokens if token and token not in SPDX_OPERATORS}


def clean_expression(raw: str) -> str:
    """The expression of an SPDX-License-Identifier line, without comment closers."""
    text = re.split(r"\*/|-->|\\n|\"", raw)[0]
    return text.strip().rstrip(",;").strip()


# --------------------------------------------------------------------- REUSE.toml


def reuse_glob(pattern: str) -> re.Pattern:
    """REUSE.toml path glob: `**` crosses directories, `*` does not, `\\*` is literal."""
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("\\*", i):
            out.append(re.escape("*"))
            i += 2
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def as_list(value) -> list[str]:
    if value is None:
        return []
    return [value] if isinstance(value, str) else list(value)


def load_reuse(repo: Path) -> list[dict]:
    data = tomllib.loads((repo / "REUSE.toml").read_text(encoding="utf-8"))
    annotations = []
    for table in data.get("annotations", []):
        annotations.append({
            "globs": [reuse_glob(p) for p in as_list(table["path"])],
            "precedence": table.get("precedence", "closest"),
            "licence": table.get("SPDX-License-Identifier", ""),
            "copyright": [f"Copyright {c}" for c in as_list(table.get("SPDX-FileCopyrightText"))],
        })  # fmt: skip
    return annotations


def reuse_match(annotations: list[dict], rel: str) -> dict | None:
    """The last matching annotation (REUSE 3.3 applies the last matching table)."""
    found = None
    for annotation in annotations:
        if any(glob.match(rel) for glob in annotation["globs"]):
            found = annotation
    return found


def header_licence(text: str) -> tuple[str, list[str]]:
    match = SPDX_LINE.search(text)
    expression = clean_expression(match.group("expr")) if match else ""
    copyrights = [clean_copyright(line) for line in COPYRIGHT_LINE.findall(text)]
    return expression, [c for c in copyrights if c]


def clean_copyright(line: str) -> str:
    text = re.split(r"\*/|-->", line)[0].strip().rstrip("\\").strip().rstrip("\"',").strip()
    text = re.sub("^" + re.escape(COPYRIGHT_TAG) + r"\s*", "Copyright ", text)
    return text if re.search(r"\d{4}|\(c\)|©", text, re.IGNORECASE) else ""


def file_licence(path: Path, rel: str, annotations: list[dict]) -> tuple[str, list[str]]:
    """(SPDX expression, copyright lines) of a repository file: its own header, or
    REUSE.toml when it has none or an override annotation matches it."""
    try:
        text = path.read_bytes()[:HEADER_BYTES].decode("utf-8", errors="replace")
    except OSError as error:
        raise LicensingError(f"cannot read {path}: {error}") from error
    expression, copyrights = header_licence(text)
    annotation = reuse_match(annotations, rel)
    if annotation and (not expression or annotation["precedence"] == "override"):
        return annotation["licence"], annotation["copyright"]
    return expression, copyrights


# ------------------------------------------------------------------ scan-build


def ninja_deps(build: Path) -> list[str]:
    """Every input `ninja -t deps` records for the objects the build compiled."""
    out = subprocess.run(
        ["ninja", "-C", str(build), "-t", "deps"],
        check=True, capture_output=True, text=True, timeout=TIMEOUT,
    ).stdout  # fmt: skip
    return sorted({line.strip() for line in out.splitlines() if line.startswith("    ")})


def classify_inputs(build: Path, repo: Path, inputs: list[str]) -> dict[str, list[str]]:
    """Split dependency paths into repository files, generated build files and the rest."""
    build_root, repo_root = build.resolve(), repo.resolve()
    groups: dict[str, set[str]] = {"repo": set(), "generated": set(), "system": set()}
    for item in inputs:
        path = Path(os.path.normpath(build_root / item))
        resolved = path.resolve()
        if resolved.is_relative_to(build_root) or path.is_relative_to(build_root):
            groups["generated"].add(path.relative_to(build_root).as_posix())
        elif resolved.is_relative_to(repo_root):
            groups["repo"].add(resolved.relative_to(repo_root).as_posix())
        else:
            groups["system"].add(str(path))
    return {name: sorted(values) for name, values in groups.items()}


def generated_rule(rules: list[dict], rel: str) -> dict:
    for rule in rules:
        if fnmatch.fnmatchcase(rel, rule["pattern"]):
            return rule
    raise LicensingError(
        f"generated build input {rel} matches no generated_build_files rule in {MANIFEST.name}"
    )


def generated_source(rule: dict, rel: str, build: Path, repo: Path) -> str:
    """The repository file a generated build input embeds: Meson copies it into the
    build directory under its own name, so the copy's bytes identify it."""
    pattern = rule["repo"].format(stem=Path(rel).name.removesuffix(".c"))
    candidates = sorted(p for p in repo.glob(pattern) if p.is_file())
    copy = build / rel.removesuffix(".c")
    if copy.is_file():
        data = copy.read_bytes()
        candidates = [p for p in candidates if p.read_bytes() == data]
    if len(candidates) != 1 and not (copy.is_file() and candidates):
        raise LicensingError(
            f"generated {rel}: {pattern} identifies {len(candidates)} files, not 1"
        )
    return candidates[0].relative_to(repo).as_posix()


def compiled_source(rule: dict, rel: str, repo: Path) -> str:
    """The repository source a generated build input was compiled from: the GPU
    kernel object a GPU build embeds (bin2c of an nvcc fatbin), named after its
    source. Exactly one repository file may match."""
    name = Path(rel).name.removesuffix(rule["suffix"])
    pattern = rule["compiled_from"].format(name=name)
    candidates = sorted(p for p in repo.glob(pattern) if p.is_file())
    if len(candidates) != 1:
        raise LicensingError(
            f"generated {rel}: {pattern} identifies {len(candidates)} files, not 1"
        )
    return candidates[0].relative_to(repo).as_posix()


def generated_entry(rule: dict, rel: str, dirs: tuple[Path, Path], annotations: list[dict]) -> dict:
    build, repo = dirs
    if "compiled_from" in rule:
        source = compiled_source(rule, rel, repo)
        expression, copyrights = file_licence(repo / source, source, annotations)
        return {"path": rel, "from": source, "licence": expression, "copyright": copyrights}
    if "repo" in rule:
        source = generated_source(rule, rel, build, repo)
        expression, copyrights = file_licence(repo / source, source, annotations)
        return {"path": rel, "from": source, "licence": expression, "copyright": copyrights}
    return {"path": rel, "licence": rule["licence"], "copyright": rule.get("copyright", [])}


def scan_build(build: Path, repo: Path, manifest: dict) -> dict:
    groups = classify_inputs(build, repo, ninja_deps(build))
    annotations = load_reuse(repo)
    files = []
    for rel in groups["repo"]:
        expression, copyrights = file_licence(repo / rel, rel, annotations)
        if not expression:
            raise LicensingError(f"compiled file {rel} has no SPDX header and no REUSE.toml entry")
        files.append({"path": rel, "licence": expression, "copyright": copyrights})
    rules = manifest["generated_build_files"]
    for rel in groups["generated"]:
        files.append(generated_entry(generated_rule(rules, rel), rel, (build, repo), annotations))
    licences = sorted(set().union(*(spdx_ids(f["licence"]) for f in files)) - {"NONE"})
    return {"schema_version": 1, "licences": licences, "files": files,
            "system_inputs": len(groups["system"])}  # fmt: skip


def go_list_files(package: str, repo: Path, own: set[str]) -> list[str]:
    """Repository files `go build` compiles or embeds for a package and its
    dependencies inside our own modules (`go list -deps -json`)."""
    result = subprocess.run(["go", "list", "-deps", "-json", package], cwd=repo, capture_output=True,
                            text=True, timeout=TIMEOUT, check=False, env=git_environment())  # fmt: skip
    if result.returncode != 0:
        raise LicensingError(f"go list {package}: {result.stderr.strip()[:300]}")
    decoder, text, pos, files = json.JSONDecoder(), result.stdout, 0, set()
    for _ in range(MAX_FILES):
        while pos < len(text) and text[pos].isspace():
            pos += 1
        if pos >= len(text):
            break
        package_info, pos = decoder.raw_decode(text, pos)
        if (package_info.get("Module") or {}).get("Path") in own:
            files |= package_files(package_info, repo)
    return sorted(files)


def package_files(info: dict, repo: Path) -> set[str]:
    base = Path(info["Dir"]).resolve()
    names = info.get("GoFiles", []) + info.get("CgoFiles", []) + info.get("EmbedFiles", [])
    return {(base / name).resolve().relative_to(repo.resolve()).as_posix() for name in names}


def scan_go(args: argparse.Namespace, manifest: dict) -> dict:
    """The build scan of our own Go files a Go program compiles, merged into an
    existing scan (the C library's) when one is given."""
    repo = Path(args.repo)
    annotations = load_reuse(repo)
    own = set(manifest.get("go_own_modules", []))
    scan = json.loads(Path(args.merge).read_text(encoding="utf-8")) if args.merge else {
        "schema_version": 1, "licences": [], "files": [], "system_inputs": 0}  # fmt: skip
    known = {entry["path"] for entry in scan["files"]}
    for package in args.package:
        for rel in go_list_files(package, repo, own):
            if rel in known:
                continue
            expression, copyrights = file_licence(repo / rel, rel, annotations)
            if not expression:
                raise LicensingError(
                    f"compiled Go file {rel} has no SPDX header and no REUSE.toml entry"
                )
            scan["files"].append({"path": rel, "licence": expression, "copyright": copyrights})
            known.add(rel)
    scan["licences"] = sorted(
        set().union(*(spdx_ids(f["licence"]) for f in scan["files"])) - {"NONE"}
    )
    return scan


# ----------------------------------------------------------------- artifact facts


def elf_build_id(path: Path) -> str | None:
    """NT_GNU_BUILD_ID of a 64-bit little-endian ELF file, or None."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    (shoff,) = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    for index in range(min(shnum, 4096)):
        base = shoff + index * shentsize
        (sh_type,) = struct.unpack_from("<I", data, base + 4)
        if sh_type != 7:  # SHT_NOTE
            continue
        offset, size = struct.unpack_from("<QQ", data, base + 0x18)
        found = note_build_id(data[offset : offset + size])
        if found:
            return found
    return None


def note_build_id(notes: bytes) -> str | None:
    pos = 0
    for _ in range(256):
        if pos + 12 > len(notes):
            return None
        namesz, descsz, kind = struct.unpack_from("<III", notes, pos)
        name_end = pos + 12 + ((namesz + 3) & ~3)
        if kind == 3 and notes[pos + 12 : pos + 12 + namesz].rstrip(b"\0") == b"GNU":
            return notes[name_end : name_end + descsz].hex()
        pos = name_end + ((descsz + 3) & ~3)
    return None


def walk_artifact(root: Path) -> list[str]:
    """Every file and symlink of the artifact tree, relative, POSIX separators."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        base = Path(dirpath)
        for name in filenames + [d for d in dirnames if (base / d).is_symlink()]:
            found.append((base / name).relative_to(root).as_posix())
            if len(found) > MAX_FILES:
                raise LicensingError(f"more than {MAX_FILES} files under {root}")
    return sorted(found)


STATUS_D = "var/lib/dpkg/status.d"


def control_fields(block: str) -> dict:
    return dict(re.findall(r"^([A-Za-z-]+): (.*)$", block, re.MULTILINE))


def dpkg_packages(root: Path) -> list[dict]:
    """Installed packages of a dpkg database (name, version, source, built-using).
    A distroless image has no `status` file: each package is one stanza in
    `status.d/<package>`, installed by being there."""
    status = root / "var/lib/dpkg/status"
    if status.is_file():
        blocks = status.read_text(encoding="utf-8").split("\n\n")
        found = [control_fields(block) for block in blocks]
        return [f for f in found if "Package" in f and "installed" in f.get("Status", "")]
    stanzas = sorted(p for p in (root / STATUS_D).glob("*") if p.suffix != ".md5sums")
    found = [control_fields(p.read_text(encoding="utf-8")) for p in stanzas if p.is_file()]
    return [f for f in found if "Package" in f]


def dpkg_owned(root: Path) -> set[str]:
    """Paths dpkg owns: `info/*.list`, or a distroless image's `status.d/*.md5sums`
    (relative paths of the package's regular files)."""
    owned: set[str] = set()
    info = root / "var/lib/dpkg/info"
    for listing in sorted(info.glob("*.list")):
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.strip() and line != "/.":
                owned.add(line.lstrip("/"))
    for sums in sorted((root / STATUS_D).glob("*.md5sums")):
        for line in sums.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split(maxsplit=1)
            if len(parts) == 2:
                owned.add(parts[1].lstrip("/"))
    return owned


def package_sources(fields: dict) -> list[str]:
    """`source=version` of a package and of its Built-Using / Static-Built-Using."""
    match = re.match(r"^(\S+)(?: \((.+)\))?$", fields.get("Source", fields["Package"]))
    specs = [f"{match.group(1)}={match.group(2) or fields['Version']}"]
    for key in ("Built-Using", "Static-Built-Using"):
        for item in fields.get(key, "").split(","):
            used = re.match(r"^\s*(\S+) \(= (.+)\)\s*$", item)
            if used:
                specs.append(f"{used.group(1)}={used.group(2)}")
    return specs


def copyright_file(root: Path, package: str) -> Path:
    name = package.split(":")[0]
    return root / "usr/share/doc" / name / "copyright"


def dist_infos(root: Path, site: str) -> list[Path]:
    base = root / site
    return sorted(base.glob("*.dist-info")) if base.is_dir() else []


def record_rows(dist: Path) -> list[tuple[str, str]]:
    """(relative path, sha256 hex or '') rows of a dist-info RECORD."""
    rows = []
    for line in (dist / "RECORD").read_text(encoding="utf-8").splitlines():
        parts = line.rsplit(",", 2)
        if len(parts) == 3 and parts[0]:
            digest = parts[1].removeprefix("sha256=")
            rows.append((parts[0].strip('"'), digest))
    return rows


def b64_sha256(path: Path) -> str:
    import base64

    digest = hashlib.sha256(path.read_bytes()).digest()
    return base64.urlsafe_b64encode(digest).decode().rstrip("=")


def metadata_licence(first: dict, fields: list[tuple[str, str]]) -> str:
    """License-Expression, else a short License field, else the licence classifiers."""
    classifiers = [v.split(" :: ")[-1] for k, v in fields if k == "Classifier" and "License" in v]
    licence = first.get("License-Expression") or first.get("License", "")[:80]
    if not licence or len(licence) > 60:
        licence = "; ".join(classifiers) or licence
    return licence


def is_licence_file(rel: Path) -> bool:
    """A licence file of a dist-info: named like one, or anywhere under the
    `licenses/` directory core metadata 2.4 (PEP 639) keeps them in."""
    return bool(LICENCE_FILE.match(rel.name)) or "licenses" in rel.parts[:-1]


def dist_metadata(dist: Path) -> dict:
    text = (dist / "METADATA").read_text(encoding="utf-8", errors="replace")
    head = text.split("\n\n", 1)[0]
    fields = re.findall(r"^([A-Za-z-]+): (.*)$", head, re.MULTILINE)
    get = {k: v for k, v in reversed(fields)}
    licence = metadata_licence(get, fields)
    files = sorted(p.relative_to(dist).as_posix() for p in dist.rglob("*")
                   if p.is_file() and is_licence_file(p.relative_to(dist)))  # fmt: skip
    return {"name": get.get("Name", dist.name), "version": get.get("Version", "?"),
            "licence": licence, "licence_files": files}  # fmt: skip


# --------------------------------------------------------------------- claiming

USRMERGE = ("bin/", "sbin/", "lib/", "lib32/", "lib64/", "libx32/")


def usrmerge_aliases(rel: str) -> tuple[str, ...]:
    for top in USRMERGE:
        if rel.startswith("usr/" + top):
            return (rel, rel[4:])
        if rel.startswith(top):
            return (rel, "usr/" + rel)
    return (rel,)


class Context:
    """What the claim rules read from the artifact and the repository."""

    def __init__(self, root: Path, repo: Path, record: dict) -> None:
        self.root, self.repo, self.record = root, repo, record
        self.dpkg = dpkg_owned(root)
        self.dpkg_dirs = {str(Path(rel).parents[i]) for rel in self.dpkg
                          for i in range(len(Path(rel).parents) - 1)}  # fmt: skip
        self.packages = dpkg_packages(root)
        self.recorded: set[str] = set()
        self.dists: list[Path] = []
        for component in record["components"]:
            for site in component.get("roots", []):
                self.add_site(site)

    def add_site(self, site: str) -> None:
        for dist in dist_infos(self.root, site):
            self.dists.append(dist)
            base = dist.parent
            for rel, _digest in record_rows(dist):
                path = Path(os.path.normpath(base / rel))
                if path.is_relative_to(self.root):
                    self.recorded.add(path.relative_to(self.root).as_posix())


def manifest_paths(component: dict, repo: Path) -> set[str]:
    spec = component.get("manifest")
    if not spec:
        return set()
    lines = (repo / spec["file"]).read_text(encoding="utf-8").splitlines()
    return {spec["prefix"] + line.split()[1] for line in lines if line.strip()}


def compiled_matchers(record: dict, repo: Path) -> list[tuple[dict, list, set[str]]]:
    matchers = []
    for component in record["components"]:
        globs = [reuse_glob(p) for p in component.get("paths", [])]
        matchers.append((component, globs, manifest_paths(component, repo)))
    return matchers


def link_target(root: Path, rel: str) -> str | None:
    """Where a symlink of the artifact points, relative to the artifact root (an
    absolute target is read inside the root), or None when it leaves the root."""
    path = root / rel
    if not path.is_symlink():
        return None
    target = os.readlink(path)
    base = Path("/") if target.startswith("/") else Path("/" + rel).parent
    resolved = os.path.normpath(base / target).lstrip("/")
    return None if resolved.startswith("..") else resolved


def dpkg_claims(ctx: Context, rel: str) -> bool:
    """A path dpkg owns, or a symlink onto one: a distroless image's `md5sums` list
    only regular files, so a package's own links (`libz.so.1`, zoneinfo aliases,
    `lib64`, `usr/share/doc/libgcc-s1`) are claimed through what they point at."""
    if any(alias in ctx.dpkg for alias in usrmerge_aliases(rel)):
        return True
    target = link_target(ctx.root, rel)
    for _ in range(8):  # bounded: a chain of links onto links
        if target is None:
            return False
        aliases = usrmerge_aliases(target)
        if any(a in ctx.dpkg or a in ctx.dpkg_dirs for a in aliases):
            return True
        target = link_target(ctx.root, target)
    return False


def claims(component: dict, globs: list, listed: set[str], ctx: Context, rel: str) -> bool:
    kind = component["kind"]
    if kind == "dpkg":
        return dpkg_claims(ctx, rel)
    if kind == "python-dist":
        return rel in ctx.recorded or in_dist_info(component, rel)
    if kind == "repo":
        return repo_path(component, rel) is not None
    if kind == "dpkg-copied":
        return copied_claims(component, ctx, rel)
    return rel in listed or any(glob.match(rel) for glob in globs)


def in_dist_info(component: dict, rel: str) -> bool:
    return ".dist-info/" in rel and any(rel.startswith(f"{site}/") for site in component["roots"])


def repo_path(component: dict, rel: str) -> str | None:
    for entry in component.get("map", []):
        prefix = entry["artifact"]
        if rel == prefix or (prefix.endswith("/") and rel.startswith(prefix)):
            return entry["repo"] + rel[len(prefix) :]
    return None


def mapped_files(component: dict, root: Path) -> list[str]:
    """Files under the artifact paths a repo component maps (notices run inside a
    live image, where walking `/` would enter /proc)."""
    found: list[str] = []
    for entry in component.get("map", []):
        path = root / entry["artifact"]
        if path.is_file():
            found.append(entry["artifact"])
        elif path.is_dir():
            found += [entry["artifact"] + rel for rel in walk_artifact(path)]
    return found


def assign(files: list[str], ctx: Context) -> tuple[dict[str, list[str]], list[str]]:
    """Owner component id per file (first claim in record order) and the unclaimed files."""
    matchers = compiled_matchers(ctx.record, ctx.repo)
    owned: dict[str, list[str]] = {c["id"]: [] for c in ctx.record["components"]}
    unclaimed = []
    for rel in files:
        owner = next((c["id"] for c, g, s in matchers if claims(c, g, s, ctx, rel)), None)
        if owner is None:
            unclaimed.append(rel)
        else:
            owned[owner].append(rel)
    return owned, unclaimed


# ------------------------------------------------------------------------ checks


def foreign_packages(record: dict) -> set[str]:
    """Packages a `dpkg-foreign` component records: installed by dpkg from a vendor's
    release rather than the distribution (the Intel GPU stack of the SYCL image). The
    component carries their licence texts and names their source."""
    return {
        name
        for component in record["components"]
        if component["kind"] == "dpkg-foreign"
        for name in component.get("packages", [])
    }


def foreign_patterns(record: dict) -> list[str]:
    """Glob patterns of a `dpkg-foreign` component (`package_patterns`): the vendor
    packages of an image whose exact package set is fixed by a base image the fork
    did not choose package by package (the already-published images, ADR-1578)."""
    return [
        pattern
        for component in record["components"]
        if component["kind"] == "dpkg-foreign"
        for pattern in component.get("package_patterns", [])
    ]


def is_foreign(package: str, names: set[str], patterns: list[str]) -> bool:
    return package in names or any(fnmatch.fnmatchcase(package, p) for p in patterns)


def check_dpkg(ctx: Context) -> list[str]:
    problems = []
    foreign = foreign_packages(ctx.record)
    patterns = foreign_patterns(ctx.record)
    installed = {fields["Package"] for fields in ctx.packages}
    for fields in ctx.packages:
        if is_foreign(fields["Package"], foreign, patterns):
            continue
        if not copyright_file(ctx.root, fields["Package"]).is_file():
            problems.append(f"package {fields['Package']} has no /usr/share/doc/*/copyright")
    problems += [f"recorded vendor package {name} is not installed"
                 for name in sorted(foreign - installed)]  # fmt: skip
    return problems


def grafted_rule(manifest: dict, name: str) -> dict | None:
    for rule in manifest["grafted_libraries"]:
        if fnmatch.fnmatchcase(name, rule["pattern"]):
            return rule
    return None


def check_grafted(ctx: Context, manifest: dict) -> list[str]:
    """Libraries auditwheel grafted into wheels: recorded, unmodified, copyleft with source."""
    problems = []
    for dist in ctx.dists:
        for rel, digest in record_rows(dist):
            if ".libs/" not in rel:
                continue
            path = Path(os.path.normpath(dist.parent / rel))
            problems += grafted_problems(manifest, path, digest)
    return problems


def grafted_problems(manifest: dict, path: Path, digest: str) -> list[str]:
    if not path.is_file():
        return []
    rule = grafted_rule(manifest, path.name)
    if rule is None:
        return [f"grafted library {path.name} has no grafted_libraries entry"]
    problems = []
    if digest and b64_sha256(path) != digest:
        problems.append(f"grafted library {path.name} differs from its wheel RECORD (modified)")
    if rule["copyleft"] and (elf_build_id(path) or "") not in rule.get("sources", {}):
        problems.append(
            f"copyleft {path.name} (build ID {elf_build_id(path)}) has no recorded source"
        )
    return problems


def vendored_rule(component: dict, name: str) -> dict | None:
    for rule in component.get("vendored_libraries", []):
        if fnmatch.fnmatchcase(name, rule["pattern"]):
            return rule
    return None


def vendored_archives(rule: dict, path: Path) -> list[str]:
    """The source archives a copyleft vendored library's build ID is recorded with."""
    archives = rule.get("sources", {}).get(elf_build_id(path) or "", [])
    return [archives] if isinstance(archives, str) else list(archives)


def check_vendored(ctx: Context, owned: dict[str, list[str]]) -> list[str]:
    """Shared libraries a vendor bundled (the ROCm runtime's `rocm_sysdeps`): every
    recorded pattern matches a file of its component, and a copyleft one's ELF build
    ID names its corresponding source."""
    problems = []
    for component in ctx.record["components"]:
        rules = component.get("vendored_libraries", [])
        files = [ctx.root / rel for rel in owned.get(component["id"], [])]
        real = [path for path in files if path.is_file() and not path.is_symlink()]
        for rule in rules:
            matched = [path for path in real if fnmatch.fnmatchcase(path.name, rule["pattern"])]
            if not matched:
                problems.append(f"vendored library {rule['pattern']} of component "
                                f"{component['id']} matches no file")  # fmt: skip
            problems += [f"copyleft {path.name} (build ID {elf_build_id(path)}) has no recorded source"
                         for path in matched if rule["copyleft"] and not vendored_archives(rule, path)]  # fmt: skip
    return problems


def dist_texts(record: dict) -> dict[str, list[dict]]:
    """Licence texts the record names for a distribution whose dist-info keeps none
    (a wheel that ships its licence inside the package, or none at all): `texts`
    entries of a python-dist component carrying `dist`, keyed by distribution name."""
    found: dict[str, list[dict]] = {}
    for component in record["components"]:
        if component["kind"] != "python-dist":
            continue
        for entry in component_texts(component):
            if "dist" in entry:
                found.setdefault(entry["dist"], []).append(entry)
    return found


def check_dists(ctx: Context) -> list[str]:
    problems = []
    recorded = dist_texts(ctx.record)
    for dist in ctx.dists:
        meta = dist_metadata(dist)
        if not meta["licence_files"] and meta["name"] not in recorded:
            problems.append(f"{dist.name} keeps no licence file")
        if not meta["licence"]:
            problems.append(f"{dist.name} declares no licence")
    return problems


def check_licences(found: set[str], component: dict, what: str) -> list[str]:
    allowed = set(component.get("licences", [])) | {"NONE"}
    extra = sorted(found - allowed)
    return (
        [f"{what} declares {', '.join(extra)}, not in component {component['id']}"] if extra else []
    )


def check_repo_files(
    component: dict, files: list[str], ctx: Context, reuse: list[dict]
) -> list[str]:
    problems = []
    for rel in files:
        source = repo_path(component, rel) or rel
        source = re.sub(r"__pycache__/([^/]+)\.cpython-\d+\.pyc$", r"\1.py", source)
        path = ctx.root / rel
        expression, _ = file_licence(path, source, reuse) if path.is_file() else ("", [])
        if not expression and path.is_file():
            problems.append(f"{rel} (from {source}) has no recorded licence")
            continue
        problems += check_licences(spdx_ids(expression), component, rel)
    return problems


# ----------------------------------------------------------------------- texts


def spdx_text_name(identifier: str) -> str:
    return f"texts/{identifier}.txt"


def component_texts(component: dict) -> list[dict]:
    return list(component.get("texts", []))


def text_target(entry: dict) -> str:
    """Path of a recorded text relative to the licence root (or the artifact root
    for texts that stay where the artifact keeps them, prefixed with '/')."""
    if "artifact" in entry:
        return "/" + entry["artifact"]
    if "fetched_dir" in entry:
        return f"texts/{entry['fetched_dir']}/"
    return f"texts/{entry['name']}"


def needed_texts(record: dict, scan: dict, manifest: dict, licences: set[str]) -> dict[str, dict]:
    """target -> source entry of every text the notices must carry."""
    needed: dict[str, dict] = {}
    for identifier in sorted(licences - {"NONE"}):
        if identifier not in manifest["spdx_texts"]:
            raise LicensingError(
                f"licence {identifier} has no text in spdx_texts of {MANIFEST.name}"
            )
        needed[spdx_text_name(identifier)] = {"repo": manifest["spdx_texts"][identifier]}
    for component in record["components"]:
        for entry in component_texts(component):
            needed[text_target(entry)] = entry
    return needed


def install_text(target: str, entry: dict, licence_root: Path, repo: Path, texts: Path) -> None:
    if target.startswith("/"):
        return
    destination = licence_root / target
    if "fetched_dir" in entry:
        shutil.copytree(texts / entry["fetched_dir"], destination, dirs_exist_ok=True)
        return
    source = texts / entry["fetched"] if "fetched" in entry else repo / entry["repo"]
    if not source.is_file():
        raise LicensingError(f"licence text {source} is missing")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def text_present(target: str, root: Path, licence_root: Path) -> bool:
    if target.startswith("/"):
        return (root / target[1:]).is_file()
    path = licence_root / target
    return path.is_dir() and any(path.iterdir()) if target.endswith("/") else path.is_file()


# --------------------------------------------------------------------- notices


def grouped_copyrights(files: list[dict]) -> dict[str, list[str]]:
    groups: dict[str, set[str]] = {}
    for entry in files:
        if entry["licence"] == "NONE":
            continue
        groups.setdefault(entry["licence"], set()).update(entry["copyright"])
    return {expr: sorted(lines) for expr, lines in sorted(groups.items())}


def repo_licences(component: dict, files: list[str], ctx: Context, reuse: list[dict]) -> list[dict]:
    entries = []
    for rel in files:
        path = ctx.root / rel
        if not path.is_file() or path.suffix == ".pyc":
            continue
        source = repo_path(component, rel) or rel
        expression, copyrights = file_licence(path, source, reuse)
        entries.append({"licence": expression or "NONE", "copyright": copyrights})
    return entries


def component_section(component: dict, files: list[dict] | None) -> list[str]:
    lines = [f"[component {component['id']}] {component['name']}"]
    if component.get("licence"):
        lines.append(f"  Licence: {component['licence']}")
    lines += [f"  {line}" for line in component.get("copyright", [])]
    if component.get("source"):
        lines.append(f"  Source: {component['source']}")
    lines += [f"  Text: {text_target(e)}  ({e.get('label', '')})".rstrip()
              for e in component_texts(component)]  # fmt: skip
    lines += copyright_groups(files or [])
    lines += [f"  Vendored {rule['pattern']}: {rule['licence']}"
              + (f"; source {', '.join(rule.get('archives', []))}" if rule["copyleft"] else "")
              for rule in component.get("vendored_libraries", [])]  # fmt: skip
    lines += [f"  Note: {note}" for note in component.get("notes", [])]
    return lines + [""]


def copyright_groups(files: list[dict]) -> list[str]:
    lines = []
    for expression, copyrights in grouped_copyrights(files).items():
        texts = ", ".join(spdx_text_name(i) for i in sorted(spdx_ids(expression)))
        lines.append(f"  {expression}  [{texts}]")
        lines.extend(f"    {line}" for line in copyrights)
    return lines


def dpkg_section(ctx: Context) -> list[str]:
    if not ctx.packages:
        return []
    header = (
        "[packages] Debian packages: each package's terms are its "
        "/usr/share/doc/<package>/copyright (the GPL and LGPL texts those files name "
        "are in /usr/share/common-licenses/)"
    )
    lines = [header]
    for fields in sorted(ctx.packages, key=lambda f: f["Package"]):
        specs = " ".join(package_sources(fields))
        lines.append(f"  {fields['Package']} {fields['Version']}  source {specs}")
    return lines + [""]


def dist_section(ctx: Context, manifest: dict) -> list[str]:
    if not ctx.dists:
        return []
    lines = ["[python] Python packages: name version, licence, licence files in the dist-info"]
    recorded = dist_texts(ctx.record)
    for dist in ctx.dists:
        meta = dist_metadata(dist)
        where = dist.relative_to(ctx.root).as_posix()
        lines.append(f"  {meta['name']} {meta['version']}: {meta['licence']}")
        lines.extend(f"    {where}/{name}" for name in meta["licence_files"])
        lines.extend(f"    {text_target(entry)}" for entry in recorded.get(meta["name"], [])
                     if not meta["licence_files"])  # fmt: skip
    lines += ["", "[grafted] Libraries bundled inside wheels (licence; source of copyleft ones)"]
    for dist in ctx.dists:
        for rel, _digest in record_rows(dist):
            path = Path(os.path.normpath(dist.parent / rel))
            if ".libs/" in rel and path.is_file():
                lines.append("  " + grafted_line(manifest, path, ctx.root))
    return lines + [""]


def grafted_line(manifest: dict, path: Path, root: Path) -> str:
    rule = grafted_rule(manifest, path.name) or {"licence": "UNRECORDED", "copyleft": False}
    line = f"{path.relative_to(root).as_posix()}: {rule['licence']}"
    if rule["copyleft"]:
        archive = rule.get("sources", {}).get(elf_build_id(path) or "", "UNRECORDED")
        line += f"; source {manifest['source_archives'].get(archive, {}).get('file', archive)}"
    return line


def notice_header(record: dict, args: argparse.Namespace) -> list[str]:
    offer = record["source_offer"].format(tag=args.tag)
    return [
        f"Licences and notices of {record['title']} {args.tag}",
        "=" * 72,
        "",
        f"VMAFx source of this artifact: https://github.com/VMAFx/vmafx/tree/{args.source_commit}",
        "(EUPL-1.2 Article 5; the licence texts named below are in texts/ next to this file).",
        f"Corresponding source of the copyleft parts: {offer}",
        "Recorded by tools/rc1-tester/image/licensing.json (ADR-1503).",
        "",
    ]


def licences_of(scan: dict, files: dict[str, list[dict]]) -> set[str]:
    found = set(scan["licences"])
    for entries in files.values():
        for entry in entries:
            found |= spdx_ids(entry["licence"])
    return found


def write_notices(args: argparse.Namespace, manifest: dict) -> None:
    record = artifact_record(manifest, args.artifact)
    root, repo = Path(args.root), Path(args.repo)
    scan = json.loads(Path(args.build_scan).read_text(encoding="utf-8"))
    ctx = Context(root, repo, record)
    reuse = load_reuse(repo)
    per_component: dict[str, list[dict]] = {}
    for component in record["components"]:
        if component["kind"] == "build":
            per_component[component["id"]] = scan["files"]
        elif component["kind"] == "repo":
            files = mapped_files(component, root)
            per_component[component["id"]] = repo_licences(component, files, ctx, reuse)
    licence_root = root / record["licence_root"]
    licence_root.mkdir(parents=True, exist_ok=True)
    for target, entry in needed_texts(
        record, scan, manifest, licences_of(scan, per_component)
    ).items():
        install_text(target, entry, licence_root, repo, Path(args.texts))
    lines = notice_header(record, args)
    for component in record["components"]:
        if component["kind"] not in {"dpkg", "dpkg-copied", "python-dist", "notices", "state"}:
            lines += component_section(component, per_component.get(component["id"]))
    if getattr(args, "go_licences", None):
        shutil.copytree(args.go_licences, licence_root / "go", dirs_exist_ok=True)
    lines += (
        copied_section(ctx)
        + dpkg_section(ctx)
        + dist_section(ctx, manifest)
        + go_section(ctx, manifest)
    )
    (licence_root / NOTICES_NAME).write_text("\n".join(lines), encoding="utf-8")
    shutil.copyfile(args.build_scan, licence_root / "vmafx-compiled-sources.json")


def run_check(args: argparse.Namespace, manifest: dict) -> list[str]:
    record = artifact_record(manifest, args.artifact)
    root, repo = Path(args.root), Path(args.repo)
    scan = json.loads(Path(args.build_scan).read_text(encoding="utf-8"))
    ctx = Context(root, repo, record)
    owned, unclaimed = assign(walk_artifact(root), ctx)
    problems = [f"no recorded licence: {rel}" for rel in unclaimed]
    problems += check_python(record, args.python_version)
    problems += check_dpkg(ctx) + check_dists(ctx) + check_grafted(ctx, manifest)
    problems += check_vendored(ctx, owned)
    problems += go_problems(ctx, manifest) + copied_problems(ctx)
    reuse = load_reuse(repo)
    per_component: dict[str, list[dict]] = {}
    for component in record["components"]:
        if component["kind"] == "build":
            problems += check_licences(set(scan["licences"]), component, "the compiled sources")
            per_component[component["id"]] = scan["files"]
        elif component["kind"] == "repo":
            problems += check_repo_files(component, owned[component["id"]], ctx, reuse)
            per_component[component["id"]] = repo_licences(
                component, owned[component["id"]], ctx, reuse
            )
    problems += check_notices(record, ctx, scan, manifest, per_component)
    return problems


def check_python(record: dict, version: str) -> list[str]:
    expected = record.get("python", {}).get("version")
    if expected and expected != version:
        message = (
            f"the artifact's interpreter is Python {version}; the record names {expected} "
            "(add its Doc/license.rst to cpython_license_rst and update the record)"
        )
        return [message]
    return []


def check_notices(record: dict, ctx: Context, scan: dict, manifest: dict,
                  per_component: dict[str, list[dict]]) -> list[str]:  # fmt: skip
    licence_root = ctx.root / record["licence_root"]
    notices = licence_root / NOTICES_NAME
    if not notices.is_file():
        return [f"{record['licence_root']}/{NOTICES_NAME} is missing"]
    text = notices.read_text(encoding="utf-8")
    problems = []
    needed = needed_texts(record, scan, manifest, licences_of(scan, per_component))
    problems += [
        f"licence text {t} is missing"
        for t in needed
        if not text_present(t, ctx.root, licence_root)
    ]
    for component in record["components"]:
        if (
            component["kind"] not in {"dpkg", "python-dist", "notices", "state"}
            and f"[component {component['id']}]" not in text
        ):
            problems.append(f"the notices do not name component {component['id']}")
    problems += [f"the notices do not list package {f['Package']}" for f in ctx.packages
                 if f"  {f['Package']} {f['Version']} " not in text]  # fmt: skip
    problems += [f"the notices do not list {d.name}" for d in ctx.dists
                 if f"  {dist_metadata(d)['name']} {dist_metadata(d)['version']}:" not in text]  # fmt: skip
    return problems


# --------------------------------------------------------------------- sources


DEP5_COPYLEFT = re.compile(r"(?i)^(?:a|l)?gpl|^mpl|^epl|^eupl|^cddl|^cpl|^osl|^sspl")
DEP5_ALTERNATIVE = re.compile(r"\s+(?:or|and)\s+|\s*,\s*")


def copyright_copyleft(text: str) -> set[str]:
    """Copyleft licences a Debian copyright file declares: the `License:` names of a
    machine-readable file, and the licence classes of any licence text it quotes."""
    found = {classify_licence_text(text)} & COPYLEFT_CLASSES
    for name in re.findall(r"^License:\s*(.+)$", text, re.MULTILINE):
        found.update(n for n in DEP5_ALTERNATIVE.split(name.strip()) if DEP5_COPYLEFT.match(n))
    return found


def copyleft_declared(ctx: Context, fields: dict) -> bool:
    """A vendor package whose copyright file declares a copyleft licence."""
    path = copyright_file(ctx.root, fields["Package"])
    if not path.is_file():
        return False
    return bool(copyright_copyleft(path.read_text(encoding="utf-8", errors="replace")))


def debian_specs(ctx: Context, record: dict) -> set[str]:
    specs: set[str] = set()
    foreign, patterns = foreign_packages(record), foreign_patterns(record)
    for fields in ctx.packages:
        # a vendor package's component names its source, unless the package's own
        # copyright file declares a copyleft licence: that source is owed (ADR-1578)
        if not is_foreign(fields["Package"], foreign, patterns) or copyleft_declared(ctx, fields):
            specs.update(package_sources(fields))
    specs |= copied_specs(ctx)
    for component in record["components"]:
        spec_file = component.get("debian_source_file")
        if spec_file:
            specs.add((ctx.root / spec_file).read_text(encoding="utf-8").strip())
    return specs


def vendored_archive_ids(ctx: Context) -> set[str]:
    """Source archives of the copyleft vendored libraries the artifact holds."""
    archives: set[str] = set()
    for component in ctx.record["components"]:
        if not component.get("vendored_libraries"):
            continue
        for rel in mapped_or_globbed(component, ctx):
            path = ctx.root / rel
            rule = vendored_rule(component, path.name)
            if rule and rule["copyleft"] and path.is_file() and not path.is_symlink():
                archives.update(vendored_archives(rule, path))
    return archives


def mapped_or_globbed(component: dict, ctx: Context) -> list[str]:
    """Files of the artifact a fixed component's path globs claim."""
    globs = [reuse_glob(pattern) for pattern in component.get("paths", [])]
    return [rel for rel in walk_artifact(ctx.root) if any(glob.match(rel) for glob in globs)]


def archive_ids(ctx: Context, manifest: dict) -> set[str]:
    archives: set[str] = vendored_archive_ids(ctx)
    for dist in ctx.dists:
        for rel, _digest in record_rows(dist):
            path = Path(os.path.normpath(dist.parent / rel))
            rule = grafted_rule(manifest, path.name) if ".libs/" in rel else None
            if rule and rule["copyleft"] and path.is_file():
                archives.add(rule["sources"][elf_build_id(path) or ""])
    return archives


def source_list(args: argparse.Namespace, manifest: dict) -> list[str]:
    """`debian <src>=<version>` and `archive <id>` lines for the artifact's copyleft code."""
    record = artifact_record(manifest, args.artifact)
    ctx = Context(Path(args.root), Path(args.repo), record)
    specs, archives = debian_specs(ctx, record), archive_ids(ctx, manifest)
    return ([f"debian {s}" for s in sorted(specs)] + [f"archive {a}" for a in sorted(archives)]
            + go_source_lines(ctx, manifest))  # fmt: skip


def retry_wait(error: urllib.error.HTTPError, attempt: int) -> float:
    """Seconds to wait before asking again: the host's Retry-After when it gives a
    number of seconds, else 2, 4, 8, ...; never more than HTTP_MAX_WAIT."""
    header = (error.headers or {}).get("Retry-After", "")
    wait = float(header) if header.strip().isdigit() else float(2 ** (attempt + 1))
    return min(wait, HTTP_MAX_WAIT)


def open_url(url: str):
    """urlopen() that asks again after a rate limit or a gateway error, a bounded
    number of times (the CI image builds share one address with many jobs)."""
    request = urllib.request.Request(url, headers={"User-Agent": "vmafx-licensing"})
    for attempt in range(HTTP_ATTEMPTS):
        try:
            return urllib.request.urlopen(request, timeout=TIMEOUT)
        except urllib.error.HTTPError as error:
            if error.code not in HTTP_RETRY_STATUS or attempt == HTTP_ATTEMPTS - 1:
                raise
            wait = retry_wait(error, attempt)
            error.close()
            print(f"licensing: {url}: HTTP {error.code}, again in {wait:.0f} s", file=sys.stderr)
            time.sleep(wait)
    raise LicensingError(f"{url}: no attempt made")


def download(url: str, destination: Path, sha256: str | None) -> None:
    digest = hashlib.sha256()
    with (
        open_url(url) as response,
        destination.open("wb") as out,
    ):
        for _ in range(MAX_DOWNLOAD // 65536 + 1):
            chunk = response.read(65536)
            if not chunk:
                break
            digest.update(chunk)
            out.write(chunk)
        else:
            raise LicensingError(f"{url} is larger than {MAX_DOWNLOAD} bytes")
    if sha256 and digest.hexdigest() != sha256:
        destination.unlink()
        raise LicensingError(f"{url}: SHA-256 {digest.hexdigest()} is not the recorded {sha256}")


def snapshot_fetch(spec: str, out: Path) -> None:
    """Debian source package files of an exact version from snapshot.debian.org."""
    name, version = spec.split("=", 1)
    api = f"https://snapshot.debian.org/mr/package/{name}/{version}/srcfiles?fileinfo=1"
    with open_url(api) as response:
        info = json.load(response)
    for entry in info.get("result", []):
        digest = entry["hash"]
        file_name = info["fileinfo"][digest][0]["name"]
        target = out / file_name
        download(f"https://snapshot.debian.org/file/{digest}", target, None)
        if (
            hashlib.sha1(target.read_bytes()).hexdigest() != digest
        ):  # snapshot.debian.org names files by SHA-1
            raise LicensingError(f"snapshot file {file_name} does not match its hash {digest}")
    if not info.get("result"):
        raise LicensingError(f"snapshot.debian.org has no source files for {spec}")


LAUNCHPAD_ARCHIVE = "https://api.launchpad.net/1.0/ubuntu/+archive/primary"


def json_get(url: str) -> dict | list:
    with open_url(url) as response:
        return json.load(response)


def launchpad_fetch(spec: str, out: Path) -> None:
    """Ubuntu source package files of an exact version from Launchpad, which keeps
    every version it published (an Ubuntu mirror keeps only the current ones). A
    superseded version is "Deleted" there, yet its files stay served (the kernel
    source of a base image's linux-libc-dev); the published entries come first, a
    deleted one is used only when its files still download, hash-verified."""
    name, version = spec.split("=", 1)
    query = urllib.parse.urlencode({"ws.op": "getPublishedSources", "source_name": name,
                                    "version": version, "exact_match": "true"})  # fmt: skip
    listing = json_get(f"{LAUNCHPAD_ARCHIVE}?{query}")
    entries = listing.get("entries", []) if isinstance(listing, dict) else []
    exact = [e for e in entries if e.get("source_package_version") == version]
    for entry in sorted(exact, key=lambda e: e.get("status") == "Deleted"):
        try:
            if launchpad_files(entry, out):
                return
        except (urllib.error.URLError, LicensingError, KeyError, ValueError):
            if entry.get("status") != "Deleted":
                raise
    raise LicensingError(f"Launchpad has no source files for {spec}")


def launchpad_files(entry: dict, out: Path) -> bool:
    """Download the files of one published-source record; False when it lists none."""
    files = json_get(f"{entry['self_link']}?ws.op=sourceFileUrls&include_meta=true")
    for item in files if isinstance(files, list) else []:
        file_name = urllib.parse.unquote(item["url"].rsplit("/", 1)[-1])
        download(item["url"], out / file_name, item["sha256"])
    return bool(files)


def fetch_debian(spec: str, out: Path) -> str:
    """One source package at its exact version: the configured apt archive, else
    snapshot.debian.org (Debian), else Launchpad (Ubuntu); fails if none has it."""
    result = subprocess.run(
        ["apt-get", "source", "--download-only", "-qq", spec],
        cwd=out, capture_output=True, text=True, timeout=TIMEOUT, check=False,
    )  # fmt: skip
    if result.returncode == 0:
        return "archive"
    failures = [f"apt-get source: {result.stderr.strip()[:120]}"]
    for where, fetch in (("snapshot", snapshot_fetch), ("launchpad", launchpad_fetch)):
        try:
            fetch(spec, out)
            return where
        except (LicensingError, urllib.error.URLError, KeyError, ValueError) as error:
            failures.append(f"{where}: {error}")
    raise LicensingError(f"no source for {spec}: " + "; ".join(failures))


COMMIT_ID = re.compile(r"[0-9a-f]{40}")


def git_environment() -> dict[str, str]:
    """The environment without the caller's GIT_* variables: run from a git hook,
    GIT_DIR and GIT_INDEX_FILE would point every command at the caller's repository."""
    return {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def git(argv: list[str], cwd: Path) -> str:
    result = subprocess.run(["git", *argv], cwd=cwd, capture_output=True, text=True,
                            timeout=TIMEOUT, check=False, env=git_environment())  # fmt: skip
    if result.returncode != 0:
        raise LicensingError(f"git {argv[0]} failed: {result.stderr.strip()[:200]}")
    return result.stdout.strip()


def fetch_git_archive(archive: dict, target: Path) -> None:
    """A recorded source tree by its git commit, the one identity a host cannot
    change under us: fetch exactly that commit, refuse anything else, and write
    `git archive` of it."""
    commit = archive["commit"]
    if COMMIT_ID.fullmatch(commit) is None:
        raise LicensingError(f"{archive['file']}: {commit!r} is not a full commit ID")
    work = target.parent / f".{target.name}.git"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        git(["init", "-q"], work)
        git(["fetch", "-q", "--depth=1", archive["git"], commit], work)
        fetched = git(["rev-parse", "FETCH_HEAD^{commit}"], work)
        if fetched != commit:
            raise LicensingError(f"{archive['git']}: fetched {fetched}, recorded {commit}")
        prefix = target.name.removesuffix(".tar.gz") + "/"
        # gzip at level 9, the strongest gzip (ADR-1591); the name stays .tar.gz.
        git(["archive", "--format=tar.gz", "-9", f"--prefix={prefix}", "-o", str(target), fetched],
            work)  # fmt: skip
    finally:
        shutil.rmtree(work, ignore_errors=True)


def fetch_archive(archive: dict, out: Path) -> str:
    """One recorded source archive into out/; its index line."""
    target = out / "archives" / archive["file"]
    if "git" in archive:
        fetch_git_archive(archive, target)
        where = f"{archive['git']} at commit {archive['commit']} (git archive)"
    else:
        download(archive["url"], target, archive["sha256"])
        where = archive["url"]
    return f"{archive['file']}  archives/  {where}  {archive['why']}"


def fetch_sources(args: argparse.Namespace, manifest: dict) -> list[str]:
    out = Path(args.out)
    (out / "debian").mkdir(parents=True, exist_ok=True)
    (out / "archives").mkdir(parents=True, exist_ok=True)
    index = []
    for line in Path(args.list).read_text(encoding="utf-8").splitlines():
        kind, value = line.split(" ", 1)
        if kind == "debian":
            index.append(f"{value}  debian/  ({fetch_debian(value, out / 'debian')})")
        elif kind == "gomod":
            index.append(fetch_go_module(value, out))
        else:
            index.append(fetch_archive(manifest["source_archives"][value], out))
    return index


def recorded_fetches(record: dict, manifest: dict) -> dict[str, dict]:
    """name -> {url, sha256} of every `fetched` text of the record other than CPython's,
    from the manifest's `fetched_texts` (a text pinned by URL and SHA-256)."""
    registry = manifest.get("fetched_texts", {})
    found: dict[str, dict] = {}
    for component in record["components"]:
        for entry in component_texts(component):
            name = entry.get("fetched")
            if name is None or name == "cpython-license.rst":
                continue
            if name not in registry:
                raise LicensingError(f"fetched text {name} has no entry in fetched_texts")
            found[name] = registry[name]
    return found


DOCX_PARAGRAPH = re.compile(r"<w:p\b[^>]*?(?:/>|>.*?</w:p>)", re.DOTALL)
DOCX_PARAGRAPH_PROPERTIES = re.compile(r"<w:pPr\b.*?</w:pPr>", re.DOTALL)
DOCX_RUN_TEXT = re.compile(r"<w:t\b[^>]*>([^<]*)</w:t>|<w:(tab|br)\b[^>]*/>")


def docx_text(path: Path) -> str:
    """The paragraphs of a Word document, in order, one per line: the text of its
    `w:t` runs, tabs and line breaks. List numbering is not reproduced."""
    try:
        with zipfile.ZipFile(path) as archive:
            document = archive.read("word/document.xml").decode("utf-8")
    except (zipfile.BadZipFile, KeyError, UnicodeDecodeError) as error:
        raise LicensingError(f"{path.name} is not a readable .docx: {error}") from error
    lines = []
    for paragraph in DOCX_PARAGRAPH.findall(document):
        runs = DOCX_PARAGRAPH_PROPERTIES.sub("", paragraph)  # its tab stops are no tabs
        parts = [
            {"tab": "\t", "br": "\n"}[tag] if tag else html.unescape(text)
            for text, tag in DOCX_RUN_TEXT.findall(runs)
        ]
        lines.append("".join(parts).rstrip())
    if not any(lines):
        raise LicensingError(f"{path.name} has no text")
    return "\n".join(lines) + "\n"


def fetch_text(name: str, spec: dict, out: Path) -> None:
    """Download one `fetched_texts` entry into out/name, checking its SHA-256. An
    entry with `"extract": "docx-text"` names a Word document (a licence published
    only as one); its text is written, headed by where it came from."""
    extract = spec.get("extract")
    if extract is None:
        download(spec["url"], out / name, spec["sha256"])
        return
    if extract != "docx-text":
        raise LicensingError(f"fetched text {name}: unknown extract {extract!r}")
    document = out / f"{name}.docx"
    download(spec["url"], document, spec["sha256"])
    header = (f"Text of {spec['url']}\n(SHA-256 {spec['sha256']}): the document's paragraphs "
              "in order, without its list numbering. The document is the authoritative form.\n\n")  # fmt: skip
    (out / name).write_text(header + docx_text(document), encoding="utf-8")
    document.unlink()


def fetch_texts(args: argparse.Namespace, manifest: dict) -> None:
    record = artifact_record(manifest, args.artifact)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    entry = manifest["cpython_license_rst"].get(args.python_version)
    if record.get("python") and entry is None:
        raise LicensingError(f"no cpython_license_rst entry for Python {args.python_version}")
    if entry:
        download(entry["url"], out / "cpython-license.rst", entry["sha256"])
    for name, spec in recorded_fetches(record, manifest).items():
        fetch_text(name, spec, out)


# ------------------------------------------------- libraries copied out of dpkg


def copied_entries(root: Path, component: dict) -> list[dict]:
    """Rows of a `dpkg-copied` component's list: a shared library copied out of a
    Debian package into an image without that package's dpkg record (the node
    image's FFmpeg dependencies). Row: `<path> <package> <version> <source>=<version>`."""
    listing = root / component["list"]
    if not listing.is_file():
        return []
    rows = []
    for line in listing.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) == 4:
            rows.append({"path": fields[0].lstrip("/"), "package": fields[1], "version": fields[2],
                         "source": fields[3]})  # fmt: skip
    return rows


def copied_problems(ctx: Context) -> list[str]:
    problems = []
    for component in ctx.record["components"]:
        if component["kind"] != "dpkg-copied":
            continue
        if not (ctx.root / component["list"]).is_file():
            problems.append(f"{component['list']} (list of component {component['id']}) is missing")
        for row in copied_entries(ctx.root, component):
            if not (ctx.root / row["path"]).exists():
                problems.append(f"copied library {row['path']} is listed but not in the artifact")
            if not (ctx.root / component["root"] / row["package"] / "copyright").is_file():
                problems.append(
                    f"copied library {row['path']}: package {row['package']} has no copyright file"
                )
    return problems


def copied_claims(component: dict, ctx: Context, rel: str) -> bool:
    if rel == component["list"] or rel.startswith(component["root"].rstrip("/") + "/"):
        return True
    return rel in {row["path"] for row in copied_entries(ctx.root, component)}


def copied_section(ctx: Context) -> list[str]:
    lines = []
    for component in ctx.record["components"]:
        if component["kind"] != "dpkg-copied":
            continue
        lines.append(f"[component {component['id']}] {component['name']}: each package's terms are "
                     f"{component['root']}/<package>/copyright")  # fmt: skip
        lines += [f"  {row['path']}: {row['package']} {row['version']}  source {row['source']}"
                  for row in copied_entries(ctx.root, component)]  # fmt: skip
        lines.append("")
    return lines


def copied_specs(ctx: Context) -> set[str]:
    return {row["source"] for component in ctx.record["components"] if component["kind"] == "dpkg-copied"
            for row in copied_entries(ctx.root, component)}  # fmt: skip


# --------------------------------------------------------------------- Go modules

GO_MAGIC = b"\xff Go buildinf:"
LICENCE_CLASSES = (  # first match wins; the text of a module's licence file
    ("AGPL-3.0", re.compile(r"GNU AFFERO GENERAL PUBLIC LICENSE")),
    ("LGPL-3.0", re.compile(r"GNU LESSER GENERAL PUBLIC LICENSE\s+Version 3")),
    ("LGPL-2.1", re.compile(r"GNU LESSER GENERAL PUBLIC LICENSE\s+Version 2\.1")),
    ("LGPL-2.0", re.compile(r"GNU LIBRARY GENERAL PUBLIC LICENSE")),
    ("GPL-3.0", re.compile(r"GNU GENERAL PUBLIC LICENSE\s+Version 3")),
    ("GPL-2.0", re.compile(r"GNU GENERAL PUBLIC LICENSE\s+Version 2")),
    ("EUPL-1.2", re.compile(r"EUROPEAN UNION PUBLIC LICEN[CS]E", re.IGNORECASE)),
    ("MPL-2.0", re.compile(r"Mozilla Public License,? [Vv]ersion 2\.0")),
    ("EPL-2.0", re.compile(r"Eclipse Public License - v 2\.0")),
    ("Apache-2.0", re.compile(r"Apache License,?\s+Version 2\.0")),
    ("MIT", re.compile(r"Permission is hereby granted, free of charge")),
    (
        "BSD-3-Clause",
        re.compile(
            r"Redistribution and use in source and binary forms[\s\S]*?(?:endorse|promote) products"
        ),
    ),
    ("BSD-2-Clause", re.compile(r"Redistribution and use in source and binary forms")),
    (
        "ISC",
        re.compile(
            r"Permission to use, copy, modify, and(?:/or)? distribute this software for any"
        ),
    ),
    (
        "Unlicense",
        re.compile(r"This is free and unencumbered software released into the public domain"),
    ),
    ("CC0-1.0", re.compile(r"CC0 1\.0 Universal|Creative Commons Legal Code\s+CC0")),
    ("BSL-1.0", re.compile(r"Boost Software License - Version 1\.0")),
    ("Zlib", re.compile(r"This software is provided 'as-is', without any express or implied")),
)
COPYLEFT_CLASSES = {
    "AGPL-3.0",
    "LGPL-3.0",
    "LGPL-2.1",
    "LGPL-2.0",
    "GPL-3.0",
    "GPL-2.0",
    "EUPL-1.2",
    "MPL-2.0",
    "EPL-2.0",
}
LINKING_CLASSES = {"AGPL-3.0", "LGPL-3.0", "LGPL-2.1", "LGPL-2.0", "GPL-3.0", "GPL-2.0"}
GO_LICENCE_FILE = re.compile(r"(?i)^(licen[cs]e|copying|unlicense|notice|patents)([._-].*)?$")


def classify_licence_text(text: str) -> str:
    for name, pattern in LICENCE_CLASSES:
        if pattern.search(text):
            return name
    return "UNKNOWN"


def elf_section(data: bytes, wanted: str) -> bytes | None:
    """Bytes of a named section of a 64-bit little-endian ELF file, or None."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    (shoff,) = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if shoff == 0 or shstrndx >= shnum:
        return None
    names_off, names_size = struct.unpack_from("<QQ", data, shoff + shstrndx * shentsize + 0x18)
    names = data[names_off : names_off + names_size]
    for index in range(min(shnum, 4096)):
        base = shoff + index * shentsize
        (name_at,) = struct.unpack_from("<I", data, base)
        name = names[name_at : names.find(b"\0", name_at)].decode("ascii", "replace")
        if name == wanted:
            offset, size = struct.unpack_from("<QQ", data, base + 0x18)
            return data[offset : offset + size]
    return None


def read_varint_string(data: bytes, pos: int) -> tuple[str, int]:
    length, shift = 0, 0
    for _ in range(10):
        byte = data[pos]
        pos += 1
        length |= (byte & 0x7F) << shift
        if not byte & 0x80:
            break
        shift += 7
    return data[pos : pos + length].decode("utf-8", "replace"), pos + length


def go_buildinfo(path: Path) -> dict | None:
    """The module information Go 1.18+ writes into `.go.buildinfo` (inline-string
    form): main module and every linked dependency, replacements applied."""
    section = elf_section(path.read_bytes(), ".go.buildinfo")
    if section is None or not section.startswith(GO_MAGIC):
        return None
    if not section[15] & 0x2:
        raise LicensingError(f"{path}: Go build info is not in the inline-string form (Go < 1.18)")
    version, pos = read_varint_string(section, 32)
    modinfo, _ = read_varint_string(section, pos)
    return parse_modinfo(version, modinfo[16:-16] if len(modinfo) >= 32 else "")


def parse_modinfo(version: str, modinfo: str) -> dict:
    info: dict = {"go": version, "main": None, "deps": []}
    for line in modinfo.splitlines():
        fields = line.split("\t")
        if fields[0] == "mod" and len(fields) >= 3:
            info["main"] = {
                "path": fields[1],
                "version": fields[2],
                "sum": fields[3] if len(fields) > 3 else "",
            }
        elif fields[0] == "dep" and len(fields) >= 3:
            info["deps"].append(
                {
                    "path": fields[1],
                    "version": fields[2],
                    "sum": fields[3] if len(fields) > 3 else "",
                }
            )
        elif fields[0] == "=>" and len(fields) >= 3 and info["deps"]:
            info["deps"][-1] = {
                "path": fields[1],
                "version": fields[2],
                "sum": fields[3] if len(fields) > 3 else "",
            }
    return info


def go_modules(path: Path, own_modules: set[str]) -> list[dict]:
    """The third-party modules a Go binary links: its dependencies, and its main
    module when that is not one of ours (a vendor program built from source)."""
    info = go_buildinfo(path)
    if info is None:
        raise LicensingError(f"{path} carries no Go build information")
    modules = list(info["deps"])
    main = info["main"]
    if main and main["path"] not in own_modules:
        modules.append(main)
    return [m for m in modules if m["path"] not in own_modules]


def module_key(module: dict) -> str:
    return f"{module['path']}@{module['version']}"


def go_module_dir(module: dict) -> Path:
    result = subprocess.run(
        ["go", "mod", "download", "-json", module_key(module)],
        capture_output=True, text=True, timeout=TIMEOUT, check=False,
        env={**git_environment(), "GOFLAGS": "-mod=mod"},
    )  # fmt: skip
    try:
        found = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise LicensingError(
            f"go mod download {module_key(module)}: {result.stderr.strip()[:200]}"
        ) from error
    if "Dir" not in found:
        raise LicensingError(
            f"go mod download {module_key(module)}: {found.get('Error', 'no directory')}"
        )
    return Path(found["Dir"])


def copy_module_texts(source: Path, target: Path) -> list[str]:
    copied = []
    for item in sorted(source.iterdir()):
        if item.is_file() and GO_LICENCE_FILE.match(item.name):
            target.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, target / item.name)
            copied.append(item.name)
    return copied


def collect_go_licences(args: argparse.Namespace, manifest: dict) -> dict:
    """Licence and notice files of every module the binaries link, copied out of
    the module cache into <out>/<module>@<version>/ (go-licences command)."""
    out = Path(args.out)
    rules = manifest.get("go_module_licences", {})
    own = set(manifest.get("go_own_modules", []))
    found: dict[str, dict] = {}
    for binary in args.binary:
        for module in go_modules(Path(binary), own):
            found.setdefault(module_key(module), module)
    for key, module in sorted(found.items()):
        same = rules.get(module["path"], {}).get("same_as")
        source = go_module_dir({"path": same, "version": module["version"]} if same else module)
        if not copy_module_texts(source, out / key):
            raise LicensingError(
                f"Go module {key} keeps no licence file; record it in go_module_licences"
            )
    return {"modules": sorted(found)}


def go_binary_files(ctx: Context) -> list[Path]:
    """The Go programs `go-binary` components name (exact paths, not globs)."""
    return [ctx.root / rel for component in ctx.record["components"]
            if component["kind"] == "go-binary" for rel in component["paths"]]  # fmt: skip


def go_problems(ctx: Context, manifest: dict) -> list[str]:
    """Every recorded Go program is in the artifact, every module it links has its
    licence texts in the licence directory, and each licence is classifiable."""
    problems = [f"recorded Go program {path.relative_to(ctx.root)} is not in the artifact"
                for path in go_binary_files(ctx) if not path.is_file()]  # fmt: skip
    licence_root = ctx.root / ctx.record["licence_root"] / "go"
    for module in shipped_go_modules(ctx, manifest):
        problems += go_module_problems(licence_root / module_key(module), module, manifest)
    return problems


def go_module_problems(texts: Path, module: dict, manifest: dict) -> list[str]:
    files = sorted(texts.glob("*")) if texts.is_dir() else []
    if not files:
        return [f"Go module {module_key(module)} has no licence text in the licence directory"]
    licence = go_module_licence(module, files, manifest)
    return (
        [f"Go module {module_key(module)}: licence not recognised; record it in go_module_licences"]
        if licence == "UNKNOWN"
        else []
    )


def go_module_licence(module: dict, files: list[Path], manifest: dict) -> str:
    recorded = manifest.get("go_module_licences", {}).get(module["path"], {}).get("licence")
    if recorded:
        return recorded
    classes = {classify_licence_text(f.read_text(encoding="utf-8", errors="replace"))
               for f in files if not re.match(r"(?i)^(notice|patents)", f.name)}  # fmt: skip
    classes.discard("UNKNOWN")
    return " AND ".join(sorted(classes)) if classes else "UNKNOWN"


def shipped_go_modules(ctx: Context, manifest: dict) -> list[dict]:
    own = set(manifest.get("go_own_modules", []))
    found: dict[str, dict] = {}
    for path in go_binary_files(ctx):
        if path.is_file():
            for module in go_modules(path, own):
                found.setdefault(module_key(module), module)
    return [found[key] for key in sorted(found)]


def go_section(ctx: Context, manifest: dict) -> list[str]:
    modules = shipped_go_modules(ctx, manifest)
    if not modules:
        return []
    licence_root = ctx.root / ctx.record["licence_root"] / "go"
    lines = ["[go] Go modules linked into the Go programs: module version, licence, texts in go/"]
    for module in modules:
        files = sorted((licence_root / module_key(module)).glob("*"))
        lines.append(
            f"  {module['path']} {module['version']}: {go_module_licence(module, files, manifest)}"
        )
    return lines + [""]


def go_source_lines(ctx: Context, manifest: dict) -> list[str]:
    """`gomod <module>@<version> <h1 sum>` for the corresponding source: every
    copyleft module, and every module of a binary that links an LGPL or GPL one
    (LGPL-3.0 4(d): the application's code in a form that can be relinked)."""
    own = set(manifest.get("go_own_modules", []))
    licence_root = ctx.root / ctx.record["licence_root"] / "go"
    lines: set[str] = set()
    for path in go_binary_files(ctx):
        if path.is_file():
            lines |= binary_source_lines(go_modules(path, own), licence_root, manifest)
    return sorted(lines)


def binary_source_lines(modules: list[dict], licence_root: Path, manifest: dict) -> set[str]:
    licences = {module_key(m): go_module_licence(m, sorted((licence_root / module_key(m)).glob("*")), manifest)
                for m in modules}  # fmt: skip
    linked_gpl = any(spdx_ids(licence) & LINKING_CLASSES for licence in licences.values())
    return {f"gomod {module_key(m)} {m['sum']}" for m in modules
            if linked_gpl or spdx_ids(licences[module_key(m)]) & COPYLEFT_CLASSES}  # fmt: skip


def module_escape(path: str) -> str:
    """The module proxy's case encoding: an upper-case letter becomes '!' + lower."""
    return re.sub(r"[A-Z]", lambda m: "!" + m.group(0).lower(), path)


def go_zip_hash(zip_path: Path) -> str:
    """golang.org/x/mod/sumdb/dirhash Hash1 of a module zip ('h1:' + base64)."""
    import base64
    import zipfile

    with zipfile.ZipFile(zip_path) as archive:
        names = sorted(n for n in archive.namelist() if not n.endswith("/"))
        summary = "".join(f"{hashlib.sha256(archive.read(n)).hexdigest()}  {n}\n" for n in names)
    return "h1:" + base64.b64encode(hashlib.sha256(summary.encode()).digest()).decode()


def fetch_go_module(value: str, out: Path) -> str:
    key, digest = value.split(" ", 1)
    path, version = key.rsplit("@", 1)
    target = out / "go" / f"{module_escape(path).replace('/', '_')}@{version}.zip"
    target.parent.mkdir(parents=True, exist_ok=True)
    download(f"https://proxy.golang.org/{module_escape(path)}/@v/{version}.zip", target, None)
    if go_zip_hash(target) != digest:
        target.unlink()
        raise LicensingError(f"{key}: module zip hash is not the binary's {digest}")
    return f"{key}  go/{target.name}  proxy.golang.org, {digest}"


# ------------------------------------------------------------------------- CLI


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = top.add_subparsers(dest="command", required=True)
    commands = {
        "scan-build": ("build", "repo", "out"),
        "fetch-texts": ("artifact", "python_version", "out"),
        "notices": ("artifact", "root", "repo", "build_scan", "texts", "source_commit", "tag"),
        "check": ("artifact", "root", "repo", "build_scan", "python_version"),
        "sources": ("artifact", "root", "repo", "out"),
        "fetch-sources": ("list", "out"),
        "go-licences": ("out",),
        "scan-go": ("repo", "out"),
    }
    for name, options in commands.items():
        command = sub.add_parser(name)
        for option in options:
            command.add_argument("--" + option.replace("_", "-"), required=True)
        if name == "check":
            command.add_argument(
                "--receipt", help="write a JSON summary here when the check passes"
            )
        if name == "notices":
            command.add_argument("--go-licences", help="output of go-licences, copied to go/")
        if name == "go-licences":
            command.add_argument("--binary", action="append", required=True, help="a Go program")
        if name == "scan-go":
            command.add_argument("--package", action="append", required=True, help="a main package")
            command.add_argument("--merge", help="an existing scan (scan-build output) to extend")
    return top


def dispatch(args: argparse.Namespace, manifest: dict) -> int:
    if args.command == "scan-build":
        scan = scan_build(Path(args.build), Path(args.repo), manifest)
        Path(args.out).write_text(json.dumps(scan, indent=1) + "\n", encoding="utf-8")
    elif args.command == "fetch-texts":
        fetch_texts(args, manifest)
    elif args.command == "notices":
        write_notices(args, manifest)
    elif args.command == "check":
        return report_check(args, run_check(args, manifest))
    elif args.command == "sources":
        Path(args.out).write_text("\n".join(source_list(args, manifest)) + "\n", encoding="utf-8")
    elif args.command == "scan-go":
        Path(args.out).write_text(
            json.dumps(scan_go(args, manifest), indent=1) + "\n", encoding="utf-8"
        )
    elif args.command == "go-licences":
        collected = collect_go_licences(args, manifest)
        (Path(args.out) / "modules.json").write_text(
            json.dumps(collected, indent=1) + "\n", encoding="utf-8"
        )
    else:
        index = fetch_sources(args, manifest)
        (Path(args.out) / "SOURCES.txt").write_text("\n".join(index) + "\n", encoding="utf-8")
    return 0


def report_check(args: argparse.Namespace, problems: list[str]) -> int:
    for problem in problems[:200]:
        print(f"licensing: {problem}", file=sys.stderr)
    if problems:
        print(f"licensing: {len(problems)} problem(s); ADR-1503, tools/rc1-tester/image/licensing.json",
              file=sys.stderr)  # fmt: skip
        return 1
    if args.receipt:
        receipt = {"artifact": args.artifact, "python": args.python_version, "result": "pass"}
        Path(args.receipt).write_text(json.dumps(receipt) + "\n", encoding="utf-8")
    print(f"licensing: every file of the {args.artifact} artifact has a recorded licence")
    return 0


def main(argv: list[str]) -> int:
    args = parser().parse_args(argv)
    try:
        return dispatch(args, load_manifest())
    except (LicensingError, OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        print(f"licensing: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
