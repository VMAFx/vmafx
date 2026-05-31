#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# check-ir-diff.sh — LLVM IR diff harness for bit-exact SIMD paths.
#
# ADR-0918. Compiles each entry under scripts/perf/ir-diff-config.yaml
# with `clang -O2 -mavx2 -mfma -emit-llvm -S`, extracts the listed
# functions, normalises non-semantic noise (file paths, !dbg metadata,
# attribute IDs, SSA renumber-safe whitespace), and diffs against the
# golden snapshot under testdata/ir-snapshots/.
#
# Modes:
#   check   (default) — exit non-zero on any drift, print side-by-side diff
#   update             — overwrite snapshots; intended for /regen-snapshots-
#                        style intentional changes
#
# Environment:
#   IR_DIFF_CLANG    clang binary to use (default: clang)
#   IR_DIFF_FILTER   if non-empty, only run entries whose source path
#                    contains this substring (substring match)
#
# Exit codes:
#   0  all snapshots match
#   1  one or more snapshots drifted (check mode)
#   2  configuration / toolchain error
#
# Usage:
#   bash scripts/perf/check-ir-diff.sh           # check (default)
#   bash scripts/perf/check-ir-diff.sh update    # rewrite snapshots
#   IR_DIFF_FILTER=psnr_hvs bash scripts/perf/check-ir-diff.sh

set -euo pipefail

MODE="${1:-check}"
CLANG="${IR_DIFF_CLANG:-clang}"
FILTER="${IR_DIFF_FILTER:-}"

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
CONFIG="${REPO_ROOT}/scripts/perf/ir-diff-config.yaml"
SNAPSHOT_DIR="${REPO_ROOT}/testdata/ir-snapshots"
TMP_DIR="$(mktemp -d -t ir-diff.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -f "$CONFIG" ]]; then
  echo "check-ir-diff: config not found: $CONFIG" >&2
  exit 2
fi

if ! command -v "$CLANG" >/dev/null 2>&1; then
  echo "check-ir-diff: clang not found ($CLANG); install LLVM ≥ 14 or set IR_DIFF_CLANG" >&2
  exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "check-ir-diff: python3 not found" >&2
  exit 2
fi

mkdir -p "$SNAPSHOT_DIR"

case "$MODE" in
  check | update) ;;
  *)
    echo "check-ir-diff: unknown mode '$MODE' (expected: check | update)" >&2
    exit 2
    ;;
esac

# Drive the per-entry loop in python so the YAML parse + IR normaliser
# stay in one place. Bash gathers env, python emits the verdicts.
PYTHONIOENCODING=utf-8 python3 - "$MODE" "$CONFIG" "$SNAPSHOT_DIR" "$TMP_DIR" "$CLANG" "$REPO_ROOT" "$FILTER" <<'PY_EOF'
import difflib
import re
import subprocess
import sys
from pathlib import Path

mode, config_path, snap_dir, tmp_dir, clang, repo_root, filt = sys.argv[1:8]
snap_dir = Path(snap_dir)
tmp_dir = Path(tmp_dir)
repo_root = Path(repo_root)


def parse_config(path):
    """Tiny YAML subset parser — avoids a PyYAML dep.

    Supports:  key: value  |  - key: value  |  nested 2-space indent.
    """
    entries = []
    cur = None
    cur_func_list = False
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped == "entries:":
                continue
            if line.startswith("  - source:"):
                if cur is not None:
                    entries.append(cur)
                cur = {
                    "source": stripped.split(":", 1)[1].strip(),
                    "cflags": [],
                    "functions": [],
                    "bit_exact": True,
                }
                cur_func_list = False
                continue
            if cur is None:
                continue
            if line.startswith("    functions:"):
                cur_func_list = True
                continue
            if line.startswith("    cflags:"):
                cur_func_list = False
                rest = stripped.split(":", 1)[1].strip()
                if rest:
                    cur["cflags"] = rest.split()
                continue
            if line.startswith("    bit_exact:"):
                cur_func_list = False
                val = stripped.split(":", 1)[1].strip().lower()
                cur["bit_exact"] = val in ("true", "yes", "1")
                continue
            if cur_func_list and line.startswith("      - "):
                fn = stripped[2:].strip()
                cur["functions"].append(fn)
                continue
    if cur is not None:
        entries.append(cur)
    return entries


def compile_to_ir(source: Path, extra_cflags, out_ir: Path):
    """Invoke clang to emit LLVM IR."""
    cmd = [
        clang,
        "-O2",
        "-mavx2",
        "-mfma",
        "-S",
        "-emit-llvm",
        f"-I{repo_root}/core/include",
        f"-I{repo_root}/core/src",
        *extra_cflags,
        "-o",
        str(out_ir),
        str(source),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
    if proc.returncode != 0:
        sys.stderr.write(
            f"check-ir-diff: clang failed for {source}:\n{proc.stderr}\n"
        )
        return False
    return True


# Function-definition extractor. LLVM IR `define` lines look like:
#   define dso_local <ret> @<name>(<args>) <attrs> {
# We capture everything from `define` to the matching closing `}` on its
# own line (LLVM emits one per function).
DEFINE_RE = re.compile(r"^define\s+.*@([A-Za-z_][A-Za-z0-9_]*)\(")


def extract_function(ir_text: str, fn_name: str):
    out = []
    in_fn = False
    for line in ir_text.splitlines():
        if not in_fn:
            m = DEFINE_RE.match(line)
            if m and m.group(1) == fn_name:
                in_fn = True
                out.append(line)
        else:
            out.append(line)
            if line == "}":
                return "\n".join(out) + "\n"
    return None


# Normalisation: strip non-semantic noise so a debug-info bump or
# clang-version bump doesn't trip the gate.
#
# - Drop !dbg / !tbaa / !range / !srcloc / !nosanitize references.
# - Drop trailing `, !dbg !NNN` etc on any instruction.
# - Drop the `attributes #N = { ... }` table (kept in the file footer).
# - Rewrite `dso_local`, `local_unnamed_addr`, `unnamed_addr` to a
#   stable canonical form (some LLVM minor versions reorder them).
# - Collapse multiple spaces, strip trailing whitespace.
META_REF_RE = re.compile(r",\s*!(dbg|tbaa|range|srcloc|nosanitize|prof|loop|noalias|alias\.scope)\s+![0-9A-Za-z_.]+")
META_DECL_RE = re.compile(r"^!\d+\s*=.*$")
ATTR_GROUP_RE = re.compile(r"^attributes #\d+ =")
MODULE_FLAGS_RE = re.compile(r"^!llvm\.")
SOURCE_FILENAME_RE = re.compile(r'^source_filename = ".*"$')
TARGET_TRIPLE_RE = re.compile(r'^target (datalayout|triple) = ".*"$')


def normalise(ir_block: str) -> str:
    out = []
    for raw_line in ir_block.splitlines():
        if META_DECL_RE.match(raw_line):
            continue
        if ATTR_GROUP_RE.match(raw_line):
            continue
        if MODULE_FLAGS_RE.match(raw_line):
            continue
        if SOURCE_FILENAME_RE.match(raw_line):
            continue
        if TARGET_TRIPLE_RE.match(raw_line):
            continue
        cleaned = META_REF_RE.sub("", raw_line)
        # Drop trailing attribute IDs `#N` on call/define lines — they
        # reference the attribute group table we strip above.
        cleaned = re.sub(r"\s+#\d+\b", "", cleaned)
        cleaned = re.sub(r"[ \t]+$", "", cleaned)
        out.append(cleaned)
    return "\n".join(out) + "\n"


def fma_count(ir_block: str) -> int:
    return len(re.findall(r"@llvm\.fma\.|@llvm\.fmuladd\.", ir_block))


def side_by_side(a: str, b: str, label_a: str, label_b: str) -> str:
    diff = difflib.unified_diff(
        a.splitlines(keepends=True),
        b.splitlines(keepends=True),
        fromfile=label_a,
        tofile=label_b,
        n=3,
    )
    return "".join(diff)


entries = parse_config(config_path)
if filt:
    entries = [e for e in entries if filt in e["source"]]

drift = 0
checked = 0
updated = 0

for entry in entries:
    src = repo_root / entry["source"]
    if not src.exists():
        print(f"WARN: configured source missing: {src}", file=sys.stderr)
        continue
    stem = src.stem
    ir_path = tmp_dir / f"{stem}.ll"
    if not compile_to_ir(src, entry["cflags"], ir_path):
        drift += 1
        continue
    ir_text = ir_path.read_text(encoding="utf-8")

    for fn in entry["functions"]:
        block = extract_function(ir_text, fn)
        if block is None:
            print(
                f"ERR: function `{fn}` not found in {entry['source']} IR",
                file=sys.stderr,
            )
            drift += 1
            continue
        norm = normalise(block)
        snap = snap_dir / f"{fn}.ll"

        if mode == "update":
            snap.parent.mkdir(parents=True, exist_ok=True)
            snap.write_text(norm, encoding="utf-8")
            print(f"UPDATED  {fn}  ({len(norm.splitlines())} lines, FMAs={fma_count(norm)})")
            updated += 1
            continue

        if not snap.exists():
            print(
                f"MISSING  {fn}  — no snapshot at {snap.relative_to(repo_root)}; "
                f"run `make ir-diff-update`",
                file=sys.stderr,
            )
            drift += 1
            continue

        ref = snap.read_text(encoding="utf-8")
        if ref == norm:
            print(f"OK       {fn}  ({len(norm.splitlines())} lines, FMAs={fma_count(norm)})")
            checked += 1
            continue

        ref_fma = fma_count(ref)
        cur_fma = fma_count(norm)
        verdict = "DRIFT" if entry["bit_exact"] else "WARN"
        print(
            f"{verdict:8s} {fn}  FMAs: ref={ref_fma} cur={cur_fma}",
            file=sys.stderr,
        )
        print(
            side_by_side(
                ref, norm,
                f"snapshot/{fn}.ll",
                f"current/{fn}.ll",
            ),
            file=sys.stderr,
        )
        if entry["bit_exact"]:
            drift += 1

if mode == "update":
    print(f"\nupdated {updated} snapshot(s) under testdata/ir-snapshots/")
    sys.exit(0)

print(f"\nchecked {checked} snapshot(s); {drift} drift(s)")
sys.exit(1 if drift else 0)
PY_EOF
