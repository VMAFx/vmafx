#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write core/test/dnn/signer_fixtures.h, the Sigstore bundles of the signer tests (ADR-2985).

Usage: python3 core/test/dnn/gen_signer_fixtures.py RELEASE_BUNDLE

RELEASE_BUNDLE is a Sigstore bundle VMAFx's supply-chain workflow wrote for a
release, for example `vmaf.bundle` of v1.0.0-rc.3
(`gh release download v1.0.0-rc.3 -R VMAFx/vmafx -p vmaf.bundle`). Its Fulcio
certificate is the accepted case. The other certificates are self-signed with
openssl and differ from an accepted one in one field each: the signer code
reads names and extensions, cosign checks signatures, so a self-signed
certificate exercises exactly the code under test.
"""

from __future__ import annotations

import base64
import hashlib
import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3]))
from scripts.lib.safe_subprocess import run

DER_LONG_LENGTH = 0x80
DER_EXTENSIONS = 0xA3

OWNER_OID = "1.3.6.1.4.1.57264.1.17"
OWNER_ID = "288567244"
WORKFLOW = "https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml"
OUT = pathlib.Path(__file__).with_name("signer_fixtures.h")

# name -> (subject alternative name extension value, owner ID or None)
SYNTHETIC = {
    "master": (f"URI:{WORKFLOW}@refs/heads/master", OWNER_ID),
    "wrong_owner": (f"URI:{WORKFLOW}@refs/tags/v1.0.0", "999999999"),
    "no_owner": (f"URI:{WORKFLOW}@refs/tags/v1.0.0", None),
    "prefix": (f"URI:https://evil.example/{WORKFLOW}@refs/tags/v1.0.0", OWNER_ID),
    "suffix": (f"URI:{WORKFLOW}@refs/heads/master/evil", OWNER_ID),
    "dot": (
        "URI:https://githubXcom/VMAFx/vmafx/.github/workflows/supply-chain.yml@refs/tags/v1.0.0",
        OWNER_ID,
    ),
    "other_workflow": (
        "URI:https://github.com/VMAFx/vmafx/.github/workflows/evil.yml@refs/heads/master",
        OWNER_ID,
    ),
    "two_names": (f"URI:{WORKFLOW}@refs/heads/master,URI:https://evil.example/", OWNER_ID),
}


def synthetic_certificate(workdir: pathlib.Path, name: str, san: str, owner: str | None) -> bytes:
    key = workdir / f"{name}.key"
    der = workdir / f"{name}.der"
    cmd = [
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "ec",
        "-pkeyopt",
        "ec_paramgen_curve:prime256v1",
        "-nodes",
        "-keyout",
        str(key),
        "-subj",
        "/CN=sigstore-test",
        "-days",
        "1",
        "-outform",
        "DER",
        "-out",
        str(der),
        "-addext",
        f"subjectAltName=critical,{san}",
    ]
    if owner is not None:
        cmd += ["-addext", f"{OWNER_OID}=ASN1:UTF8String:{owner}"]
    run(cmd, allowed_executables={"openssl"}, check=True, capture_output=True)
    return der.read_bytes()


def tlv(tag: int, value: bytes) -> bytes:
    size = len(value)
    length = (
        bytes([size])
        if size < DER_LONG_LENGTH
        else bytes([0x80 | ((size.bit_length() + 7) // 8)])
        + size.to_bytes((size.bit_length() + 7) // 8, "big")
    )
    return bytes([tag]) + length + value


def parts(data: bytes) -> list[tuple[int, bytes]]:
    result = []
    offset = 0
    while offset < len(data):
        tag = data[offset]
        size = data[offset + 1]
        start = offset + 2
        if size & 0x80:
            count = size & 0x7F
            size = int.from_bytes(data[start : start + count], "big")
            start += count
        result.append((tag, data[start : start + size]))
        offset = start + size
    return result


def encoded(items: list[tuple[int, bytes]]) -> bytes:
    return b"".join(tlv(tag, value) for tag, value in items)


def edge_certificate(der: bytes, case: str) -> bytes:
    # OpenSSL rejects duplicate extensions and cannot emit malformed DER lengths;
    # assemble the extensions by hand to exercise uniqueness and DER minimality.
    cert = parts(der)[0][1]
    fields = parts(cert)
    tbs = parts(fields[0][1])
    ext_index = next(i for i, item in enumerate(tbs) if item[0] == DER_EXTENSIONS)
    extensions = parts(parts(tbs[ext_index][1])[0][1])
    owner_oid = bytes.fromhex("2b0601040183bf300111")
    san_oid = bytes.fromhex("551d11")
    owner = next(i for i, (_, value) in enumerate(extensions) if parts(value)[0][1] == owner_oid)
    san = next(i for i, (_, value) in enumerate(extensions) if parts(value)[0][1] == san_oid)
    if case in ("duplicate_owner", "duplicate_san"):
        extensions.append(extensions[owner if case == "duplicate_owner" else san])
    elif case.startswith("owner_"):
        values = {
            "owner_long": b"2885672440",
            "owner_short": b"28856724",
            "owner_empty": b"",
            "owner_space": b" 288567244",
        }
        ext = parts(extensions[owner][1])
        ext[-1] = (
            4,
            tlv(0x13 if case == "owner_tag" else 0x0C, values.get(case, OWNER_ID.encode())),
        )
        extensions[owner] = (0x30, encoded(ext))
    elif case.startswith("san_"):
        ext = parts(extensions[san][1])
        names = parts(parts(ext[-1][1])[0][1])
        tag, value = names[0]
        tag = {"san_email": 0x81, "san_dns": 0x82}.get(case, tag)
        if case == "san_nul":
            value += b"\x00"
        ext[-1] = (4, tlv(0x30, tlv(tag, value)))
        extensions[san] = (0x30, encoded(ext))
    if case == "oversize":
        extensions.append((0x30, tlv(6, bytes.fromhex("2a03040506")) + tlv(4, bytes(8192))))
    ext_bytes = encoded(extensions)
    if case == "nonminimal_length":
        # DER requires short-form length for the five-byte OID of an unknown extension.
        ext_bytes += tlv(0x30, b"\x06\x81\x05\x2a\x03\x04\x05\x06\x04\x00")
    tbs[ext_index] = (0xA3, tlv(0x30, ext_bytes))
    fields[0] = (0x30, encoded(tbs))
    result = tlv(0x30, encoded(fields))
    if case == "trailing_bytes":
        result += b"\x00"
    return result


EDGE_CASES = (
    "duplicate_owner",
    "duplicate_san",
    "owner_long",
    "owner_short",
    "owner_empty",
    "owner_space",
    "owner_tag",
    "san_email",
    "san_dns",
    "san_nul",
    "nonminimal_length",
    "trailing_bytes",
)


def bundle_json(der: bytes) -> str:
    return json.dumps(
        {
            "mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json",
            "verificationMaterial": {"certificate": {"rawBytes": base64.b64encode(der).decode()}},
        },
        separators=(",", ":"),
    )


def full_bundle_json(der: bytes) -> str:
    bundle = json.loads(bundle_json(der))
    bundle["verificationMaterial"]["tlogEntries"] = [
        {
            "logIndex": "1",
            "logId": {"keyId": "AA=="},
            "kindVersion": {"kind": "hashedrekord", "version": "0.0.1"},
            "integratedTime": "1",
            "inclusionPromise": {"signedEntryTimestamp": "AA=="},
            "inclusionProof": {
                "logIndex": "1",
                "rootHash": "AA==",
                "treeSize": "1",
                "hashes": [],
                "checkpoint": {"envelope": "checkpoint\nline\n"},
            },
            "canonicalizedBody": "AA==",
        }
    ]
    bundle["messageSignature"] = {
        "messageDigest": {"algorithm": "SHA2_256", "digest": "AA=="},
        "signature": "AA==",
    }
    return json.dumps(bundle, separators=(",", ":"))


def c_literal(text: str, width: int = 90) -> str:
    parts = [text[i : i + width] for i in range(0, len(text), width)]
    return "\n".join('    "' + p.replace("\\", "\\\\").replace('"', '\\"') + '"' for p in parts)


def main() -> int:
    args = sys.argv[1:]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    release_path = pathlib.Path(args[0])
    release = json.loads(release_path.read_text(encoding="utf-8"))
    release_der = base64.b64decode(release["verificationMaterial"]["certificate"]["rawBytes"])
    digest = hashlib.sha256(release_path.read_bytes()).hexdigest()

    bundles = {"release": bundle_json(release_der), "full_shape": full_bundle_json(release_der)}
    with tempfile.TemporaryDirectory() as tmp:
        for name, (san, owner) in SYNTHETIC.items():
            der = synthetic_certificate(pathlib.Path(tmp), name, san, owner)
            bundles[name] = bundle_json(der)
            if name == "master":
                oversized = edge_certificate(der, "oversize")
                for case in EDGE_CASES:
                    bundles[case] = bundle_json(edge_certificate(der, case))

    lines = [
        "/**",
        " *  Copyright 2026 Lusoris",
        # Split so REUSE does not read this string as this script's licence line.
        " *  SPDX-" + "License-Identifier: EUPL-1.2",
        " *",
        " *  Generated by core/test/dnn/gen_signer_fixtures.py; do not edit (ADR-2985).",
        f" *  `release` holds the Fulcio certificate of {release_path.name} (sha256 {digest[:16]}),",
        " *  the others self-signed certificates that differ from an accepted one in one field.",
        " */",
        "",
        "#ifndef VMAF_TEST_DNN_SIGNER_FIXTURES_H_",
        "#define VMAF_TEST_DNN_SIGNER_FIXTURES_H_",
        "",
    ]
    for name, text in bundles.items():
        lines.append(f"static const char k_bundle_{name}[] =")
        lines.append(c_literal(text) + ";")
        lines.append("")
    lines.append("static const unsigned char k_certificate_oversize[] = {")
    for offset in range(0, len(oversized), 16):
        chunk = oversized[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* VMAF_TEST_DNN_SIGNER_FIXTURES_H_ */")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT} ({len(bundles)} bundles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
