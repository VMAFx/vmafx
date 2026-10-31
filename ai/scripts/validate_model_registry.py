#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Validate ``model/tiny/registry.json`` against ``registry.schema.json``.

T6-9 / ADR-0209 added formal license + Sigstore-bundle metadata to the
registry. This script is the gate that keeps the registry consistent with
the schema *and* with the on-disk artifacts (sha256s match, sidecars
present, bundle paths well-formed). It is wired into CI as a required
status check; running it locally before pushing avoids a CI round-trip.

Two validators run in sequence:

1. **JSON Schema** (``jsonschema``, a declared dependency; the script exits 2
   when it is not installed rather than validate less).
2. **Cross-file consistency** — every ``onnx`` exists, the recorded
   ``sha256`` matches the file on disk, every non-smoke entry has a
   sidecar JSON, ``int8_sha256`` is present iff ``quant_mode`` is not
   ``fp32``, and ``sigstore_bundle`` paths are well-formed (file presence
   is *not* required at lint time — bundles are generated at release).

Exit status: 0 = pass, 1 = validation failed, 2 = bad invocation.
"""

from __future__ import annotations

import argparse
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
from aiutils.onnx_signature import OnnxSignature, OnnxWireError, read_signature  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

DEFAULT_REGISTRY = REPO_ROOT / "model" / "tiny" / "registry.json"
DEFAULT_SCHEMA = REPO_ROOT / "model" / "tiny" / "registry.schema.json"


JSONSCHEMA_LOCK = "requirements/locks/jsonschema.txt"


class JsonschemaMissingError(RuntimeError):
    """``jsonschema`` is not importable; the validator refuses to run without it."""


def _jsonschema_errors(reg: dict[str, Any], schema: dict[str, Any]) -> list[str]:
    """Validate against the schema (Draft 2020-12); return error strings (empty = ok).

    ``jsonschema`` is a declared dependency (``pyproject.toml``, the CI job
    installs ``requirements/locks/jsonschema.txt``). There is no weaker
    stand-in: a run without it is an error, never a pass on fewer checks.
    """
    try:
        import jsonschema  # type: ignore[import-not-found]
    except ImportError as exc:
        raise JsonschemaMissingError(
            "the 'jsonschema' package is required and is not installed; install it with "
            f"`pip install --require-hashes -r {JSONSCHEMA_LOCK}` (no fallback validator runs)"
        ) from exc
    validator = jsonschema.Draft202012Validator(schema)
    errors: list[str] = []
    for err in sorted(validator.iter_errors(reg), key=lambda e: list(e.absolute_path)):
        path = "/".join(str(p) for p in err.absolute_path) or "<root>"
        errors.append(f"schema: {path}: {err.message}")
    return errors


def graph_bakes_scaler(onnx_path: Path) -> bool:
    """True when the ONNX graph applies the StandardScaler itself.

    A sidecar that declares ``"onnx_has_scaler": true`` tells the C runtime
    (``core/src/libvmaf.c``) *not* to normalise the feature vector before
    inference, because the graph already carries the ``Sub`` (mean) and
    ``Div`` (std) Constant nodes. When the two disagree the runtime
    double-scales and the score is garbage — the failure mode recorded as
    ``T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04``.

    Detection prefers the ``onnx`` protobuf parser (exact ``op_type``
    match). CI legs that install neither ``onnx`` nor the AI extras fall
    back to a length-prefixed protobuf byte scan for the ``Sub`` / ``Div``
    ``op_type`` strings (``0x22`` = field 4 ``op_type``, ``0x03`` = length).
    """

    def _byte_scan() -> bool:
        raw = onnx_path.read_bytes()
        return (b"\x22\x03Sub" in raw) and (b"\x22\x03Div" in raw)

    try:
        import onnx  # type: ignore[import-not-found]
    except ImportError:
        return _byte_scan()

    try:
        model = onnx.load(str(onnx_path), load_external_data=False)
    except Exception:
        # A file that is not parseable as ONNX is a registry error in its own
        # right, but it is not THIS function's error to raise: the caller is
        # mid-way through collecting every consistency problem, and an
        # exception here throws away the rest of the report -- including the
        # sha256 mismatch that is usually the real reason the file is wrong.
        # Degrade to the same byte scan the no-onnx-installed path uses; on
        # arbitrary bytes it simply finds no Sub/Div markers and answers False,
        # letting _consistency_check finish and report the actual mismatch.
        return _byte_scan()

    ops = {node.op_type for node in model.graph.node}
    return "Sub" in ops and "Div" in ops


def sidecar_for(onnx_path: Path) -> Path:
    """Companion sidecar for an ONNX file.

    ``foo.int8.onnx`` prefers ``foo.int8.json`` and falls back to the fp32
    sidecar ``foo.json`` — the loader (``vmaf_dnn_sidecar_load``) resolves
    the same way.
    """
    direct = onnx_path.with_suffix(".json")
    if direct.is_file():
        return direct
    name = onnx_path.name
    if name.endswith(".int8.onnx"):
        return onnx_path.with_name(name[: -len(".int8.onnx")] + ".json")
    return direct


def _check_quant_consistency(
    m: dict[str, Any], mid: str, onnx_path: Path, errors: list[str]
) -> None:
    quant_mode = m.get("quant_mode", "fp32")
    if quant_mode == "fp32":
        return
    int8_sha = m.get("int8_sha256")
    if not int8_sha:
        errors.append(f"{mid}: quant_mode={quant_mode} requires int8_sha256")
        return
    int8_path = onnx_path.with_suffix("").with_suffix(".int8.onnx")
    if not int8_path.is_file():
        return
    got8 = hashlib.sha256(int8_path.read_bytes()).hexdigest()
    if got8 != int8_sha:
        errors.append(f"{mid}: int8_sha256 mismatch (file={got8}, registry={int8_sha})")
    if not graph_bakes_scaler(int8_path):
        return
    sidecar8 = sidecar_for(int8_path)
    if not sidecar8.is_file():
        errors.append(
            f"{mid}: {int8_path.name} bakes the scaler but no companion "
            f"sidecar ({sidecar8.name}) exists to declare onnx_has_scaler"
        )
        return
    try:
        sdata8 = json.loads(sidecar8.read_text(encoding="utf-8"))
    except ValueError as err:
        errors.append(f"{mid}: {sidecar8.name} JSON parse error: {err}")
    else:
        if sdata8.get("onnx_has_scaler") is not True:
            errors.append(
                f"{mid}: {int8_path.name} bakes scaler ops but "
                f"{sidecar8.name} does not declare onnx_has_scaler: true"
            )


def _rank2_width(sig: OnnxSignature, slot: int) -> int | None:
    """Width of graph input @p slot when it is a rank-2 [batch, N] tensor."""
    if slot >= len(sig.inputs):
        return None
    dims = sig.inputs[slot].dims
    if len(dims) != 2 or not isinstance(dims[1], int):
        return None
    return dims[1]


def _sidecar_name_errors(label: str, sdata: dict[str, Any], sig: OnnxSignature) -> list[str]:
    """Tensor names the sidecar states that the graph does not have."""
    errors: list[str] = []
    names = {
        "input": [t.name for t in sig.inputs],
        "output": [t.name for t in sig.outputs],
    }
    for io, actual in names.items():
        listed = sdata.get(f"{io}_names")
        if listed is not None and list(listed) != actual:
            errors.append(f"{label}: {io}_names {listed} but the graph's {io}s are {actual}")
        single = sdata.get(f"{io}_name")
        if single is not None and single not in actual:
            errors.append(f"{label}: {io}_name {single!r} is not a graph {io} ({actual})")
    return errors


def _sidecar_width_errors(label: str, sdata: dict[str, Any], sig: OnnxSignature) -> list[str]:
    """Feature-list and codec-block widths that differ from the graph inputs."""
    errors: list[str] = []
    features = sdata.get("feature_order") or sdata.get("features")
    width = _rank2_width(sig, 0)
    if features is not None and width is not None and len(features) != width:
        errors.append(f"{label}: {len(features)} feature names for a {width}-wide input")
    codec = _rank2_width(sig, 1)
    vocab = sdata.get("encoder_vocab")
    if vocab is not None and codec != len(vocab) + 2:
        errors.append(
            f"{label}: encoder_vocab of {len(vocab)} entries describes a codec block of "
            f"{len(vocab) + 2} slots, the graph's second input has {codec}"
        )
    if "codec_block_dim" in sdata and sdata["codec_block_dim"] != codec:
        errors.append(f"{label}: codec_block_dim {sdata['codec_block_dim']}, graph {codec}")
    layout = sdata.get("codec_block_layout")
    if layout is not None and len(layout) != codec:
        errors.append(f"{label}: codec_block_layout has {len(layout)} slots, graph {codec}")
    return errors


def _sidecar_graph_errors(mid: str, graph_path: Path, sig: OnnxSignature) -> list[str]:
    """Hold the companion sidecar of @p graph_path to the graph it describes."""
    sidecar = sidecar_for(graph_path)
    if not sidecar.is_file():
        return []
    try:
        sdata = json.loads(sidecar.read_text(encoding="utf-8"))
    except ValueError as err:
        return [f"{mid}: {sidecar.name} JSON parse error: {err}"]
    label = f"{mid}: {sidecar.name}"
    errors: list[str] = []
    if "opset" in sdata and sdata["opset"] != sig.default_opset:
        errors.append(
            f"{label}: opset {sdata['opset']} but {graph_path.name} imports "
            f"opset {sig.default_opset}"
        )
    # The sha256 of a sidecar names its own graph; an int8 file that falls
    # back to the fp32 sidecar is not that graph.
    if "sha256" in sdata and sidecar.stem == graph_path.stem:
        got = hashlib.sha256(graph_path.read_bytes()).hexdigest()
        if sdata["sha256"] != got:
            errors.append(f"{label}: sha256 {sdata['sha256']} but {graph_path.name} is {got}")
    return (
        errors + _sidecar_name_errors(label, sdata, sig) + _sidecar_width_errors(label, sdata, sig)
    )


def _check_graph_metadata(m: dict[str, Any], mid: str, onnx_path: Path, errors: list[str]) -> None:
    """Registry opset and sidecar metadata against the shipped graphs.

    Reads every graph with ``aiutils.onnx_signature`` (no ``onnx`` needed),
    the fp32 file and, for a quantised entry, its int8 sibling.
    """
    graphs = [onnx_path]
    int8_path = onnx_path.with_suffix("").with_suffix(".int8.onnx")
    if m.get("quant_mode", "fp32") != "fp32" and int8_path.is_file():
        graphs.append(int8_path)
    for graph_path in graphs:
        try:
            sig = read_signature(graph_path)
        except (OSError, OnnxWireError, UnicodeDecodeError) as err:
            errors.append(f"{mid}: cannot read {graph_path.name} as ONNX: {err}")
            continue
        opset = m.get("opset")
        if opset is not None and sig.default_opset != opset:
            errors.append(
                f"{mid}: registry opset {opset} but {graph_path.name} imports "
                f"opset {sig.default_opset}"
            )
        errors.extend(_sidecar_graph_errors(mid, graph_path, sig))


def _find_model_file(teacher: str, registry_dir: Path) -> Path | None:
    """Find ``model/**/<teacher>.json``."""
    candidates = []
    if registry_dir.name == "tiny" and registry_dir.parent.name == "model":
        candidates.append(registry_dir.parent)
    candidates.extend(
        [
            registry_dir / "model",
            registry_dir.parent / "model",
            registry_dir,
            REPO_ROOT / "model",
        ]
    )
    for root in candidates:
        if root.is_dir():
            matches = [p for p in root.glob(f"**/{teacher}.json") if p.is_file()]
            if matches:
                return matches[0]
    return None


def _check_teacher_model(
    m: dict[str, Any], mid: str, registry_dir: Path, errors: list[str]
) -> None:
    """Check required teacher field and verify real model existence under model/."""
    if "teacher" not in m:
        errors.append(f"{mid}: missing required field 'teacher'")
        return
    teacher = m["teacher"]
    if not isinstance(teacher, str):
        errors.append(f"{mid}: teacher must be a string")
        return
    if teacher in ("none", "unknown"):
        return
    if not _find_model_file(teacher, registry_dir):
        errors.append(f"{mid}: teacher model {teacher!r} not found under model/")


def _consistency_check(reg: dict[str, Any], registry_dir: Path) -> list[str]:
    """Cross-file invariants the schema cannot express (file existence, sha match)."""
    errors: list[str] = []
    seen_ids: set[str] = set()
    for idx, m in enumerate(reg.get("models", [])):
        mid = m.get("id", f"<index {idx}>")
        if mid in seen_ids:
            errors.append(f"{mid}: duplicate model id")
        seen_ids.add(mid)

        _check_teacher_model(m, mid, registry_dir, errors)

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

        _check_quant_consistency(m, mid, onnx_path, errors)
        if got == want:
            _check_graph_metadata(m, mid, onnx_path, errors)

        bundle_rel = m.get("sigstore_bundle")
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
    try:
        errors.extend(_jsonschema_errors(reg, schema))
    except JsonschemaMissingError as err:
        return 2, [str(err)]
    errors.extend(_consistency_check(reg, registry_path.parent))

    return (0 if not errors else 1, errors)


def _build_parser() -> argparse.ArgumentParser:
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
    return parser


def _write_report(
    out_json: Path,
    args: argparse.Namespace,
    raw_argv: list[str],
    rc: int,
    errors: list[str],
    model_count: int,
) -> None:
    write_manifest_json(
        out_json,
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
                outputs={"report": out_json},
            ),
        },
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _build_parser().parse_args(raw_argv)

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
        _write_report(args.out_json, args, raw_argv, rc, errors, model_count)
    return rc


if __name__ == "__main__":
    sys.exit(main())
