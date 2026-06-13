#!/usr/bin/env bash
# test_registry.sh — verify model/tiny/registry.json integrity.
#
# Failure modes caught:
#   1. An ONNX listed in the registry is missing from model/tiny/.
#   2. Its on-disk sha256 does not match the registry entry.
#   3. A sidecar JSON is missing for a non-smoke entry.
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

if errors:
    for e in errors:
        print("FAIL:", e, file=sys.stderr)
    sys.exit(1)

print(f"OK: {len(reg['models'])} registry entries verified")
PY
