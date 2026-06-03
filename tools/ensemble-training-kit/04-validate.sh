#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Step 04: Validate per-seed LOSO artefacts and emit PROMOTE.json /
# HOLD.json.
#
# Wraps ai/scripts/validate_ensemble_seeds.py. Prints a one-line
# verdict + per-seed PLCCs to stdout; the verdict JSON lands at
# $OUT_DIR/{PROMOTE,HOLD}.json.

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/runs/ensemble_v2_real}"

if [[ ! -d "$OUT_DIR" ]]; then
  echo "[validate] error: $OUT_DIR not found" >&2
  exit 2
fi

# validate_ensemble_seeds.py exits 0 on PROMOTE, 1 on HOLD, 2 on input error.
set +e
python3 "$REPO_ROOT/ai/scripts/validate_ensemble_seeds.py" "$OUT_DIR"
rc=$?
set -e

# Print per-seed PLCCs from each loso_seed{N}.json (independent of the
# verdict file path, so the operator sees the raw numbers either way).
echo
echo "[validate] per-seed mean_plcc:"
for seed_json in "$OUT_DIR"/loso_seed*.json; do
  [[ -f "$seed_json" ]] || continue
  python3 -c "
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
print(f'  seed={d.get(\"seed\")} mean_plcc={d.get(\"mean_plcc\"):.4f} '
      f'spread={(d.get(\"max_plcc\", 0) - d.get(\"min_plcc\", 0)):.4f} '
      f'n_folds={d.get(\"n_folds\")}')
" "$seed_json"
done

if [[ "$rc" -eq 0 ]]; then
  echo "[validate] verdict: PROMOTE -> $OUT_DIR/PROMOTE.json"
elif [[ "$rc" -eq 1 ]]; then
  echo "[validate] verdict: HOLD -> $OUT_DIR/HOLD.json"
else
  echo "[validate] verdict: error (rc=$rc)" >&2
fi
exit "$rc"
