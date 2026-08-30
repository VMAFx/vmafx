#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Step 05: Bundle the LOSO verdict + per-seed ONNX exports for return
# transport.
#
# If PROMOTE.json exists, also re-fits + exports each seed on the FULL
# corpus via ai/scripts/export_ensemble_v2_seeds.py (the LOSO trainer
# itself discards models after PLCC computation, so the export step
# fits final production weights against the gate-passed corpus).
#
# Output: lawrence-ensemble-results-<utc-ts>.tar.gz with a manifest.

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/runs/ensemble_v2_real}"
CORPUS_JSONL="${CORPUS_JSONL:-$REPO_ROOT/runs/phase_a/full_grid/per_frame_canonical6.jsonl}"
BUNDLE_DEST="${BUNDLE_DEST:-$REPO_ROOT}"

ts="$(date -u +%Y%m%dT%H%M%SZ)"
bundle="$BUNDLE_DEST/lawrence-ensemble-results-${ts}.tar.gz"

verdict="HOLD"
if [[ -f "$OUT_DIR/PROMOTE.json" ]]; then
  verdict="PROMOTE"
fi
echo "[bundle] verdict=$verdict out_dir=$OUT_DIR"

# If PROMOTE: export per-seed ONNX members against the full corpus.
exports_dir="$OUT_DIR/exports"
mkdir -p "$exports_dir"
if [[ "$verdict" == "PROMOTE" ]]; then
  if [[ ! -f "$CORPUS_JSONL" ]]; then
    echo "[bundle] error: PROMOTE but corpus missing at $CORPUS_JSONL" >&2
    exit 2
  fi
  echo "[bundle] exporting per-seed ONNX members via export_ensemble_v2_seeds.py"
  # The export script writes to model/tiny/ by default; redirect via
  # --out-dir so the operator's repo state is left untouched and the
  # exports collect under the run directory for bundling.
  python3 "$REPO_ROOT/ai/scripts/export_ensemble_v2_seeds.py" \
    --corpus "$CORPUS_JSONL" \
    --promote-json "$OUT_DIR/PROMOTE.json" \
    --seeds 0,1,2,3,4 \
    --out-dir "$exports_dir" ||
    {
      echo "[bundle] warning: export_ensemble_v2_seeds.py failed; bundling LOSO artefacts only" >&2
    }
fi

# Manifest --------------------------------------------------------------------
manifest="$OUT_DIR/manifest.json"
python3 - "$OUT_DIR" "$exports_dir" "$verdict" "$manifest" <<'PY'
import hashlib, json, pathlib, sys
out_dir = pathlib.Path(sys.argv[1])
exports_dir = pathlib.Path(sys.argv[2])
verdict = sys.argv[3]
manifest_path = pathlib.Path(sys.argv[4])

def sha(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

entries = []
for p in sorted(out_dir.rglob("*")):
    if p.is_file():
        rel = p.relative_to(out_dir).as_posix()
        entries.append({"path": rel, "size": p.stat().st_size, "sha256": sha(p)})

manifest = {
    "schema_version": 1,
    "verdict": verdict,
    "generated_at_utc": __import__("datetime").datetime.utcnow().isoformat(timespec="seconds") + "Z",
    "files": entries,
    "n_files": len(entries),
}
manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
print(f"[manifest] wrote {len(entries)} entries -> {manifest_path}")
PY

# Tar everything under OUT_DIR (verdict, per-seed JSONs, logs, exports, manifest).
tar -czf "$bundle" -C "$(dirname "$OUT_DIR")" "$(basename "$OUT_DIR")"
size=$(du -h "$bundle" | cut -f1)
echo "[bundle] wrote $bundle ($size)"
echo "[bundle] verdict=$verdict — see manifest.json inside the tarball for the file list"
