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
             recorded archives with their SHA-256)

The record is licensing.json next to this file.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path

import tomllib

MANIFEST = Path(__file__).with_name("licensing.json")
NOTICES_NAME = "THIRD_PARTY_NOTICES.txt"
HEADER_BYTES = 6000
MAX_FILES = 400_000
MAX_DOWNLOAD = 512 * 1024 * 1024
TIMEOUT = 300
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
    return json.loads(path.read_text(encoding="utf-8"))


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


def dpkg_packages(root: Path) -> list[dict]:
    """Installed packages of a dpkg database (name, version, source, built-using)."""
    status = root / "var/lib/dpkg/status"
    if not status.is_file():
        return []
    packages = []
    for block in status.read_text(encoding="utf-8").split("\n\n"):
        fields = dict(re.findall(r"^([A-Za-z-]+): (.*)$", block, re.MULTILINE))
        if "Package" in fields and "installed" in fields.get("Status", ""):
            packages.append(fields)
    return packages


def dpkg_owned(root: Path) -> set[str]:
    owned: set[str] = set()
    info = root / "var/lib/dpkg/info"
    for listing in sorted(info.glob("*.list")):
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.strip() and line != "/.":
                owned.add(line.lstrip("/"))
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


def dist_metadata(dist: Path) -> dict:
    text = (dist / "METADATA").read_text(encoding="utf-8", errors="replace")
    head = text.split("\n\n", 1)[0]
    fields = re.findall(r"^([A-Za-z-]+): (.*)$", head, re.MULTILINE)
    get = {k: v for k, v in reversed(fields)}
    licence = metadata_licence(get, fields)
    files = sorted(p.relative_to(dist).as_posix() for p in dist.rglob("*")
                   if p.is_file() and LICENCE_FILE.match(p.name))  # fmt: skip
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


def claims(component: dict, globs: list, listed: set[str], ctx: Context, rel: str) -> bool:
    kind = component["kind"]
    if kind == "dpkg":
        return any(alias in ctx.dpkg for alias in usrmerge_aliases(rel))
    if kind == "python-dist":
        return rel in ctx.recorded or in_dist_info(component, rel)
    if kind == "repo":
        return repo_path(component, rel) is not None
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
        for name in component["packages"]
    }


def check_dpkg(ctx: Context) -> list[str]:
    problems = []
    foreign = foreign_packages(ctx.record)
    installed = {fields["Package"] for fields in ctx.packages}
    for fields in ctx.packages:
        if fields["Package"] in foreign:
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


def check_dists(ctx: Context) -> list[str]:
    problems = []
    for dist in ctx.dists:
        meta = dist_metadata(dist)
        if not meta["licence_files"]:
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
    for dist in ctx.dists:
        meta = dist_metadata(dist)
        where = dist.relative_to(ctx.root).as_posix()
        lines.append(f"  {meta['name']} {meta['version']}: {meta['licence']}")
        lines.extend(f"    {where}/{name}" for name in meta["licence_files"])
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
        if component["kind"] not in {"dpkg", "python-dist", "notices", "state"}:
            lines += component_section(component, per_component.get(component["id"]))
    lines += dpkg_section(ctx) + dist_section(ctx, manifest)
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


def debian_specs(ctx: Context, record: dict) -> set[str]:
    specs: set[str] = set()
    foreign = foreign_packages(record)
    for fields in ctx.packages:
        if fields["Package"] not in foreign:  # its component names the vendor's source
            specs.update(package_sources(fields))
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
    return [f"debian {s}" for s in sorted(specs)] + [f"archive {a}" for a in sorted(archives)]


def download(url: str, destination: Path, sha256: str | None) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "vmafx-licensing"})
    digest = hashlib.sha256()
    with (
        urllib.request.urlopen(request, timeout=TIMEOUT) as response,
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
    with urllib.request.urlopen(api, timeout=TIMEOUT) as response:
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


def fetch_debian(spec: str, out: Path) -> str:
    result = subprocess.run(
        ["apt-get", "source", "--download-only", "-qq", spec],
        cwd=out, capture_output=True, text=True, timeout=TIMEOUT, check=False,
    )  # fmt: skip
    if result.returncode == 0:
        return "archive"
    snapshot_fetch(spec, out)
    return "snapshot"


def fetch_sources(args: argparse.Namespace, manifest: dict) -> list[str]:
    out = Path(args.out)
    (out / "debian").mkdir(parents=True, exist_ok=True)
    (out / "archives").mkdir(parents=True, exist_ok=True)
    index = []
    for line in Path(args.list).read_text(encoding="utf-8").splitlines():
        kind, value = line.split(" ", 1)
        if kind == "debian":
            index.append(f"{value}  debian/  ({fetch_debian(value, out / 'debian')})")
        else:
            archive = manifest["source_archives"][value]
            download(archive["url"], out / "archives" / archive["file"], archive["sha256"])
            index.append(f"{archive['file']}  archives/  {archive['url']}  {archive['why']}")
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
        download(spec["url"], out / name, spec["sha256"])


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
    }
    for name, options in commands.items():
        command = sub.add_parser(name)
        for option in options:
            command.add_argument("--" + option.replace("_", "-"), required=True)
        if name == "check":
            command.add_argument(
                "--receipt", help="write a JSON summary here when the check passes"
            )
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
