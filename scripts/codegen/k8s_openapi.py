#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Kubernetes types of the chart's values schema (ADR-2350 D13).

Usage::

    python3 scripts/codegen/k8s_openapi.py --write
    python3 scripts/codegen/k8s_openapi.py --check

The chart's values schema refers to Kubernetes types (`k8s:<name>` in the
`[[chart]]` entries of api/vmafx-platform.toml) as the minimum Kubernetes
minor the chart supports defines them. build-config.env pins that release
(K8S_SCHEMA_VERSION) and the SHA-256 of each OpenAPI v3 file read from its
tag (K8S_SCHEMA_SHA256_*). This script downloads the files, refuses a
digest that differs, takes every type the definition names and every type
those reach, drops what JSON Schema validation does not read (`description`
and the `x-kubernetes-*` extensions) and writes the types, with their
references rewritten to `#/$defs/<name>`, to api/kubernetes/openapi-subset.json.
scripts/codegen/vmafx-api.py copies from there into values.schema.json.

It also writes the chart's THIRD-PARTY-NOTICES.txt: the attribution for those
types and the Apache-2.0 text, the licence of the Kubernetes sources
(REUSE.toml, ADR-2673). --check compares both with fresh ones; it exits 77
(Meson's skip) when the files cannot be downloaded.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path
from typing import Any

import tomllib

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sqlc_generate import build_config
from vmafx_api.chart import K8S_PREFIX, MAX_SCHEMA_NODES, parse_chart, schema_refs
from vmafx_api.emit_chart import layout

REPO = Path(__file__).resolve().parents[2]
SUBSET = Path("api/kubernetes/openapi-subset.json")
NOTICE = Path("deploy/helm/vmafx/THIRD-PARTY-NOTICES.txt")
APACHE_TEXT = Path("LICENSES/Apache-2.0.txt")
DEFINITION = Path("api/vmafx-platform.toml")
SKIP = 77
DOWNLOAD_TIMEOUT_S = 120
MAX_FILE_BYTES = 16 << 20
SOURCE_URL = (
    "https://raw.githubusercontent.com/kubernetes/kubernetes/v{version}/api/openapi-spec/v3/{file}"
)
# build-config.env key suffix -> the OpenAPI v3 file of that API group version.
FILES = {
    "CORE_V1": "api__v1_openapi.json",
    "APPS_V1": "apis__apps__v1_openapi.json",
    "NETWORKING_V1": "apis__networking.k8s.io__v1_openapi.json",
}
COMPONENTS = "#/components/schemas/"
SHA256_HEX = 64  # hex digits of a SHA-256 digest
DEFS = "#/$defs/"


class Unavailable(Exception):
    """The pinned files cannot be downloaded here; --check is skipped."""


def pins(root: Path) -> tuple[str, dict[str, str]]:
    config = build_config(root / "build-config.env")
    version = config.get("K8S_SCHEMA_VERSION", "")
    digests = {key: config.get(f"K8S_SCHEMA_SHA256_{key}", "") for key in FILES}
    if not version or not all(len(d) == SHA256_HEX for d in digests.values()):
        raise SystemExit("k8s_openapi: build-config.env needs K8S_SCHEMA_VERSION and every digest")
    return version, digests


def download(version: str, key: str, digest: str) -> dict[str, Any]:
    """One pinned OpenAPI file's `components.schemas`, digest checked."""
    url = SOURCE_URL.format(version=version, file=FILES[key])
    try:
        with urllib.request.urlopen(url, timeout=DOWNLOAD_TIMEOUT_S) as resp:  # noqa: S310
            data = resp.read(MAX_FILE_BYTES + 1)
    except OSError as exc:
        raise Unavailable(f"cannot download {url}: {exc}") from exc
    if len(data) > MAX_FILE_BYTES:
        raise SystemExit(f"k8s_openapi: {url} is larger than {MAX_FILE_BYTES} bytes")
    got = hashlib.sha256(data).hexdigest()
    if got != digest:
        raise SystemExit(f"k8s_openapi: {url} has SHA-256 {got}, build-config.env pins {digest}")
    schemas: dict[str, Any] = json.loads(data)["components"]["schemas"]
    return schemas


def merged(version: str, digests: dict[str, str]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in FILES:
        for name, schema in download(version, key, digests[key]).items():
            if name in out and out[name] != schema:
                raise SystemExit(f"k8s_openapi: {name} differs between the specification files")
            out[name] = schema
    return out


def cleaned(schema: Any) -> Any:
    """A copy without `description` and `x-kubernetes-*`, references as `#/$defs/`."""
    copy = json.loads(json.dumps(schema))
    pending = [copy]
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return copy
        node = pending.pop()
        if isinstance(node, dict):
            for key in [k for k in node if k == "description" or k.startswith("x-kubernetes-")]:
                del node[key]
            ref = node.get("$ref")
            if isinstance(ref, str) and ref.startswith(COMPONENTS):
                node["$ref"] = DEFS + ref[len(COMPONENTS) :]
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    raise SystemExit(f"k8s_openapi: a schema has more than {MAX_SCHEMA_NODES} nodes")


def roots(root: Path) -> list[str]:
    """The Kubernetes types the definition's chart entries name."""
    with (root / DEFINITION).open("rb") as handle:
        chart = parse_chart(tomllib.load(handle))
    if chart is None:
        return []
    schemas = [chart.root_schema, *(s for _, s in chart.defs)]
    schemas += [e.schema for e in chart.entries if e.schema is not None]
    refs = {
        r[len(K8S_PREFIX) :] for s in schemas for r in schema_refs(s) if r.startswith(K8S_PREFIX)
    }
    return sorted(refs)


def subset(names: list[str], schemas: dict[str, Any]) -> dict[str, Any]:
    found: dict[str, Any] = {}
    pending = list(names)
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return {name: found[name] for name in sorted(found)}
        name = pending.pop()
        if name in found:
            continue
        if name not in schemas:
            raise SystemExit(f"k8s_openapi: Kubernetes {name} is not in the pinned specification")
        found[name] = cleaned(schemas[name])
        pending += [r[len(DEFS) :] for r in schema_refs(found[name])]
    raise SystemExit(f"k8s_openapi: more than {MAX_SCHEMA_NODES} types")


def text(root: Path) -> str:
    version, digests = pins(root)
    document = {
        "kubernetes": version,
        "sources": [
            {"url": SOURCE_URL.format(version=version, file=FILES[k]), "sha256": digests[k]}
            for k in FILES
        ],
        "transform": "types the chart names and the types they reach; description and "
        "x-kubernetes-* removed; references rewritten to #/$defs/ (scripts/codegen/k8s_openapi.py)",
        "schemas": subset(roots(root), merged(version, digests)),
    }
    out: str = layout(document)
    return out


NOTICE_HEAD = """Third-party notices for the vmafx Helm chart
============================================

values.schema.json contains type schemas from the Kubernetes OpenAPI v3
specification of kubernetes/kubernetes v{version}
(https://github.com/kubernetes/kubernetes/tree/v{version}/api/openapi-spec/v3):
the definitions under $defs named io.k8s.*, with their descriptions and
x-kubernetes-* extensions removed and their references rewritten
(scripts/codegen/k8s_openapi.py, ADR-2350 D13, ADR-2673).

Copyright The Kubernetes Authors.
Licensed under the Apache License, Version 2.0, whose text follows.

"""


def notice(root: Path) -> str:
    """The chart's THIRD-PARTY-NOTICES.txt: attribution and the Apache-2.0 text."""
    version, _ = pins(root)
    licence = (root / APACHE_TEXT).read_text(encoding="utf-8")
    return NOTICE_HEAD.format(version=version) + licence


def outputs(root: Path) -> dict[Path, str]:
    return {SUBSET: text(root), NOTICE: notice(root)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="rewrite the subset and notice")
    group.add_argument("--check", action="store_true", help="fail when either differs")
    parser.add_argument("--root", type=Path, default=REPO, help="repository root")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        want = outputs(root)
    except Unavailable as exc:
        if args.check:
            print(f"k8s_openapi: SKIP: {exc}")
            return SKIP
        raise SystemExit(f"k8s_openapi: {exc}") from exc
    stale = []
    for path, content in want.items():
        target = root / path
        have = target.read_text(encoding="utf-8") if target.exists() else ""
        if content == have:
            continue
        stale.append(path)
        if args.write:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
            print(f"k8s_openapi: wrote {path}")
    if args.check and stale:
        for path in stale:
            print(f"k8s_openapi: {path} differs from the pinned release and the definition")
        print("  run python3 scripts/codegen/k8s_openapi.py --write")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
