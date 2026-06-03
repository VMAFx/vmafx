#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Validate ``model/tiny/registry.json`` against ``registry.schema.json``.

T6-9 / ADR-0209 added formal license + Sigstore-bundle metadata to the
registry. This script is the gate that keeps the registry consistent with
the schema *and* with the on-disk artifacts (sha256s match, sidecars
present, bundle paths well-formed). It is wired into CI as a required
status check; running it locally before pushing avoids a CI round-trip.

Two validators run in sequence:

1. **JSON Schema** (``jsonschema`` if installed; fall back to a small
   structural validator otherwise so distros without ``python-jsonschema``
   still get coverage of the *required-fields* invariants).
2. **Cross-file consistency** — every ``onnx`` exists, the recorded
   ``sha256`` matches the file on disk, every non-smoke entry has a
   sidecar JSON, ``int8_sha256`` is present iff ``quant_mode`` is not
   ``fp32``, and ``sigstore_bundle`` paths are well-formed (file presence
   is *not* required at lint time — bundles are generated at release).

Exit status: 0 = pass, 1 = validation failed, 2 = bad invocation.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

DEFAULT_REGISTRY = REPO_ROOT / "model" / "tiny" / "registry.json"
DEFAULT_SCHEMA = REPO_ROOT / "model" / "tiny" / "registry.schema.json"


def _try_jsonschema_validate(reg: dict[str, Any], schema: dict[str, Any]) -> list[str]:
    """Run jsonschema if available; return a list of error strings (empty = ok)."""
    try:
        import jsonschema  # type: ignore[import-not-found]
    except ImportError:
        return ["__skipped__"]
    validator = jsonschema.Draft202012Validator(schema)
    errors: list[str] = []
    for err in sorted(validator.iter_errors(reg), key=lambda e: list(e.absolute_path)):
        path = "/".join(str(p) for p in err.absolute_path) or "<root>"
        errors.append(f"schema: {path}: {err.message}")
    return errors


def _structural_fallback_validate(reg: dict[str, Any]) -> list[str]:
    """Minimal required-field check when ``jsonschema`` is unavailable."""
    errors: list[str] = []
    if not isinstance(reg, dict):
        return ["registry must be a JSON object"]
    if reg.get("schema_version") not in (0, 1):
        errors.append(f"schema_version must be 0 or 1 (got {reg.get('schema_version')!r})")
    models = reg.get("models")
    if not isinstance(models, list):
        return ["registry.models must be a list"]
    required = {"id", "kind", "onnx", "sha256"}
    for idx, m in enumerate(models):
        if not isinstance(m, dict):
            errors.append(f"models[{idx}] is not an object")
            continue
        missing = required - m.keys()
        if missing:
            errors.append(f"models[{idx}] missing required fields: {sorted(missing)}")
        kind = m.get("kind")
        if kind not in {"fr", "nr", "filter"}:
            errors.append(f"models[{idx}].kind = {kind!r} (expected fr/nr/filter)")
        sha = m.get("sha256", "")
        if not (
            isinstance(sha, str) and len(sha) == 64 and all(c in "0123456789abcdef" for c in sha)
        ):
            errors.append(f"models[{idx}].sha256 must be 64 lowercase hex chars")
    return errors


def _consistency_check(reg: dict[str, Any], registry_dir: Path) -> list[str]:
    """Cross-file invariants the schema cannot express (file existence, sha match)."""
    errors: list[str] = []
    seen_ids: set[str] = set()
    for idx, m in enumerate(reg.get("models", [])):
        mid = m.get("id", f"<index {idx}>")
        if mid in seen_ids:
            errors.append(f"{mid}: duplicate model id")
        seen_ids.add(mid)

        onnx_rel = m.get("onnx", "")
        if not onnx_rel:
            continue
        onnx_path = registry_dir / onnx_rel
        if not onnx_path.is_file():
            errors.append(f"{mid}: missing ONNX file {onnx_path}")
            continue
        got = hashlib.sha256(onnx_path.read_bytes()).hexdigest()
        want = m.get("sha256", "")
        if got != want:
            errors.append(f"{mid}: sha256 mismatch (file={got}, registry={want})")

        if not m.get("smoke", False):
            sidecar = onnx_path.with_suffix(".json")
            if not sidecar.is_file():
                errors.append(f"{mid}: missing sidecar {sidecar.name}")

        quant_mode = m.get("quant_mode", "fp32")
        if quant_mode != "fp32":
            int8_sha = m.get("int8_sha256")
            if not int8_sha:
                errors.append(f"{mid}: quant_mode={quant_mode} requires int8_sha256")
            else:
                int8_path = onnx_path.with_suffix("").with_suffix(".int8.onnx")
                if int8_path.is_file():
                    got8 = hashlib.sha256(int8_path.read_bytes()).hexdigest()
                    if got8 != int8_sha:
                        errors.append(
                            f"{mid}: int8_sha256 mismatch (file={got8}, registry={int8_sha})"
                        )

        bundle_rel = m.get("sigstore_bundle")
        # Bundle file presence is checked at runtime by --tiny-model-verify,
        # not here — release-time signing populates the file. We just
        # enforce the path-shape rule.
        if bundle_rel and not bundle_rel.endswith(".sigstore.json"):
            errors.append(
                f"{mid}: sigstore_bundle must end with .sigstore.json (got {bundle_rel!r})"
            )
    return errors


def validate(registry_path: Path, schema_path: Path) -> tuple[int, list[str]]:
    if not registry_path.is_file():
        return 2, [f"registry not found: {registry_path}"]
    if not schema_path.is_file():
        return 2, [f"schema not found: {schema_path}"]
    try:
        reg = json.loads(registry_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as err:
        return 1, [f"registry JSON parse error: {err}"]
    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as err:
        return 2, [f"schema JSON parse error: {err}"]

    errors: list[str] = []
    schema_errors = _try_jsonschema_validate(reg, schema)
    if schema_errors == ["__skipped__"]:
        errors.extend(_structural_fallback_validate(reg))
    else:
        errors.extend(schema_errors)
    errors.extend(_consistency_check(reg, registry_path.parent))

    return (0 if not errors else 1, errors)


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    parser = make_argument_parser(description=__doc__)
    parser.add_argument(
        "registry",
        nargs="?",
        type=Path,
        default=DEFAULT_REGISTRY,
        help=f"path to registry.json (default: {DEFAULT_REGISTRY})",
    )
    parser.add_argument(
        "--schema",
        type=Path,
        default=DEFAULT_SCHEMA,
        help=f"path to registry.schema.json (default: {DEFAULT_SCHEMA})",
    )
    parser.add_argument(
        "--out-json",
        type=Path,
        default=None,
        help="Optional JSON validation report with ADR-0661 run provenance.",
    )
    args = parser.parse_args(raw_argv)

    rc, errors = validate(args.registry, args.schema)
    model_count = 0
    if rc != 0:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        print(f"\n{len(errors)} error(s) — registry validation failed.", file=sys.stderr)
    else:
        try:
            model_count = len(
                json.loads(args.registry.read_text(encoding="utf-8")).get("models", [])
            )
            print(f"OK: {model_count} registry entries valid against {args.schema.name}")
        except (OSError, json.JSONDecodeError, UnicodeDecodeError) as exc:
            print(
                f"ERROR: registry validated successfully but count read failed: {exc}",
                file=sys.stderr,
            )
            rc = 1
            errors = [f"registry count read failed: {exc}"]
    if args.out_json is not None:
        write_manifest_json(
            args.out_json,
            {
                "ok": rc == 0,
                "error_count": len(errors),
                "errors": errors,
                "model_count": model_count,
                "run_provenance": build_run_provenance(
                    entrypoint=SCRIPT_PATH,
                    repo_root=REPO_ROOT,
                    argv=raw_argv,
                    args=args,
                    inputs={"registry": args.registry, "schema": args.schema},
                    outputs={"report": args.out_json},
                ),
            },
        )
    return rc


if __name__ == "__main__":
    sys.exit(main())
