#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# check-cuda-extern-c.sh — every __global__ kernel the host looks up by name
# with cuModuleGetFunction must be defined inside an extern "C" block
# (ADR-0747): C++ linkage mangles the symbol and the lookup fails at runtime.
#
# Exit 0 = every located kernel is wrapped. Exit 1 = at least one is not.
#
# Usage:
#   bash scripts/dev/check-cuda-extern-c.sh [repo-root]
#
# Algorithm:
#   1. Take the kernel-name string literal of every cuModuleGetFunction call
#      in the CUDA host sources, including calls split across lines.
#   2. Find each kernel's __global__ definition in the .cu / .cuh sources,
#      with comments and string literals blanked so braces in them do not
#      count.
#   3. The definition is wrapped when an extern "C" { opened before it has not
#      been closed yet.
#
# A kernel whose name is built by a macro (`name##suffix`) has no literal
# definition to find. Such names are listed as "not located" rather than
# passed silently; confirm those from the built PTX (`cuobjdump -ptx`).

set -euo pipefail

ROOT="${1:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"

exec python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
dirs = [root / "core/src/feature/cuda", root / "core/src/cuda"]
host = [p for d in dirs for p in d.rglob("*") if p.suffix in {".c", ".cpp", ".h"}]
device = [p for d in dirs for p in d.rglob("*") if p.suffix in {".cu", ".cuh"}]

CALL = re.compile(r'cuModuleGetFunction\s*\([^;"]*"([A-Za-z_]\w*)"')
BLANK = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"', re.S)
EXTERN_C = re.compile(r'extern\s+"C"\s*\{')


def blank(text: str) -> str:
    """Comments and string literals replaced by spaces, newlines kept."""
    return BLANK.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


names = sorted({m.group(1) for p in host for m in CALL.finditer(p.read_text(errors="replace"))})
if not names:
    print("check-cuda-extern-c: no cuModuleGetFunction calls found; nothing to check.")
    sys.exit(0)

sources = {p: p.read_text(errors="replace") for p in device}
failed, located, missing = [], 0, []
for name in names:
    definition = re.compile(r"__global__[^;{]*?\b" + re.escape(name) + r"\s*\(")
    hits = []
    for path, text in sources.items():
        for m in definition.finditer(text):
            hits.append((path, text, m.start()))
    if not hits:
        missing.append(name)
        continue
    located += 1
    for path, text, pos in hits:
        # Blanking keeps every offset, so extern "C" (whose "C" is a string
        # literal) is found in the source and the braces are counted in the
        # blanked copy, where comments and strings cannot add any.
        code = blank(text[:pos])
        opens = [m.end() for m in EXTERN_C.finditer(text[:pos]) if code[m.start()] == "e"]
        wrapped = False
        for start in opens:
            depth = 1 + code.count("{", start) - code.count("}", start)
            if depth > 0:
                wrapped = True
                break
        if not wrapped:
            line = text.count("\n", 0, pos) + 1
            failed.append(f"{path.relative_to(root)}:{line}: {name}")

for entry in failed:
    print(f'ERROR: __global__ kernel not inside extern "C" {{ }}: {entry}', file=sys.stderr)
if missing:
    print(
        f"check-cuda-extern-c: {len(missing)} looked-up kernel(s) not located as a literal "
        f"definition (macro-generated?): {', '.join(missing)}"
    )
print(f"check-cuda-extern-c: {located} of {len(names)} looked-up kernels located; {len(failed)} unwrapped.")
sys.exit(1 if failed else 0)
PY
