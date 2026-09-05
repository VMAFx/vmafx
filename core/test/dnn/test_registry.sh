#!/usr/bin/env bash
# test_registry.sh — verify model/tiny/registry.json integrity.
#
# Failure modes caught:
#   1. An ONNX listed in the registry is missing from model/tiny/.
#   2. Its on-disk sha256 does not match the registry entry.
#   3. A sidecar JSON is missing for a non-smoke entry.
#   4. An `.int8.onnx` graph bakes the StandardScaler but its sidecar does
#      not declare `"onnx_has_scaler": true` (double-scaling at runtime).
#
# This is a cheap gate — O(# of registry entries) — but it locks the
# tree-in state of every shipped tiny model. Tampering with a .onnx
# without updating registry.json will fail CI.
set -eu

TINY_DIR="${TINY_DIR:-model/tiny}"
REG="$TINY_DIR/registry.json"

if [[ ! -r "$REG" ]]; then
  echo "registry not found at $REG — run from repo root" >&2
  exit 77
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 required for registry parsing" >&2
  exit 77
fi

python3 - <<PY
import hashlib
import json
import sys
from pathlib import Path

tiny = Path("$TINY_DIR")
reg  = json.loads((tiny / "registry.json").read_text())

errors = []
for m in reg.get("models", []):
    mid = m["id"]
    onnx = tiny / m["onnx"]
    if not onnx.is_file():
        errors.append(f"{mid}: missing {onnx}")
        continue
    got = hashlib.sha256(onnx.read_bytes()).hexdigest()
    want = m["sha256"]
    if got != want:
        errors.append(f"{mid}: sha256 mismatch (got {got}, registry {want})")
        continue
    if not m.get("smoke", False):
        side = onnx.with_suffix(".json")
        if not side.is_file():
            errors.append(f"{mid}: missing sidecar {side}")

# T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04:
# an int8 graph that bakes the StandardScaler (Sub = mean, Div = std) must be
# accompanied by a sidecar declaring "onnx_has_scaler": true, otherwise
# core/src/libvmaf.c normalises the feature vector a second time and the score
# is garbage (measured: 16.02 instead of 71.95 on the src01 pair).
def graph_bakes_scaler(path):
    """Prefer the onnx parser; fall back to a protobuf byte scan.

    0x22 = protobuf field 4 (NodeProto.op_type), 0x03 = string length.
    """
    try:
        import onnx
    except ImportError:
        raw = path.read_bytes()
        return (b"\x22\x03Sub" in raw) and (b"\x22\x03Div" in raw)
    ops = {n.op_type for n in onnx.load(str(path), load_external_data=False).graph.node}
    return "Sub" in ops and "Div" in ops


for int8_onnx in sorted(tiny.glob("*.int8.onnx")):
    if not graph_bakes_scaler(int8_onnx):
        continue
    sidecar = int8_onnx.with_suffix(".json")
    if not sidecar.is_file():
        base_stem = int8_onnx.name[: -len(".int8.onnx")]
        sidecar = int8_onnx.with_name(base_stem + ".json")
    if not sidecar.is_file():
        errors.append(f"{int8_onnx.name}: bakes scaler ops but companion sidecar is missing")
        continue
    try:
        sdata = json.loads(sidecar.read_text(encoding="utf-8"))
    except ValueError as e:
        errors.append(f"{sidecar.name}: failed to parse json: {e}")
        continue
    if sdata.get("onnx_has_scaler") is not True:
        errors.append(
            f"{int8_onnx.name}: graph bakes scaler ops but {sidecar.name} "
            f"does not declare onnx_has_scaler: true"
        )

if errors:
    for e in errors:
        print("FAIL:", e, file=sys.stderr)
    sys.exit(1)

print(f"OK: {len(reg['models'])} registry entries verified")
PY
