#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Generate a portable tarball that lawrence (or any collaborator) can
# untar without cloning the vmaf fork. Bundles the kit itself, the
# scripts it invokes, the Python modules they import, and the runbook.
#
# Usage:
#   bash tools/ensemble-training-kit/make-distribution-tarball.sh [out.tar.gz]
#
# Default output path: vmaf-ensemble-training-kit-<utc-ts>.tar.gz in CWD.

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"

ts="$(date -u +%Y%m%dT%H%M%SZ)"
out="${1:-$PWD/vmaf-ensemble-training-kit-${ts}.tar.gz}"

# Files / directories to include in the bundle, repo-relative.
includes=(
  "tools/ensemble-training-kit"
  "scripts/dev/hw_encoder_corpus.py"
  "ai/scripts/run_ensemble_v2_real_corpus_loso.sh"
  "ai/scripts/train_fr_regressor_v2_ensemble_loso.py"
  "ai/scripts/train_fr_regressor_v2_ensemble.py"
  "ai/scripts/validate_ensemble_seeds.py"
  "ai/scripts/export_ensemble_v2_seeds.py"
  "ai/scripts/merge_corpora.py"
  "scripts/ci/ensemble_prod_gate.py"
  "ai/src"
  "ai/pyproject.toml"
  "ai/README.md"
  "model/tiny/registry.json"
  "docs/ai/ensemble-v2-real-corpus-retrain-runbook.md"
)

# Verify everything exists before tar-ing.
missing=()
for rel in "${includes[@]}"; do
  if [[ ! -e "$REPO_ROOT/$rel" ]]; then
    missing+=("$rel")
  fi
done
if [[ "${#missing[@]}" -gt 0 ]]; then
  echo "[dist] error: missing repo paths:" >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 2
fi

# Stage the bundle in a temp dir so the tarball has a single top-level
# directory ("vmaf-ensemble-training-kit/").
stage="$(mktemp -d -t vmaf-ekit-XXXXXX)"
trap 'rm -rf "$stage"' EXIT
top="$stage/vmaf-ensemble-training-kit"
mkdir -p "$top"

for rel in "${includes[@]}"; do
  dest="$top/$rel"
  mkdir -p "$(dirname "$dest")"
  cp -a "$REPO_ROOT/$rel" "$dest"
done

# Drop a top-level pointer to the kit's README so the operator finds
# the entry point on first untar.
cat >"$top/README-FIRST.txt" <<'EOF'
vmaf-ensemble-training-kit — portable Phase-A + LOSO retrain bundle
ADR-0324 (VMAFx/vmafx fork).

Start here:
  tools/ensemble-training-kit/README.md

Quick run (after the prereqs in that README are met):
  bash tools/ensemble-training-kit/run-full-pipeline.sh --ref-dir /path/to/netflix/ref

Send back to the lead user:
  lawrence-ensemble-results-<ts>.tar.gz (emitted by step 05)
EOF

# Compute a deterministic file list manifest for the bundle itself.
(cd "$stage" && find vmaf-ensemble-training-kit -type f | sort >vmaf-ensemble-training-kit/BUNDLE_MANIFEST.txt)

tar -czf "$out" -C "$stage" vmaf-ensemble-training-kit
size=$(du -h "$out" | cut -f1)
n=$(tar -tzf "$out" | wc -l)
echo "[dist] wrote $out ($size, $n entries)"
echo "[dist] verify: tar -tzf '$out' | head"
