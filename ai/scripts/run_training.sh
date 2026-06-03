#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Convenience wrapper for the Netflix-corpus tiny-AI training entry
# point. ADR-0203. Honours $VMAF_DATA_ROOT, $VMAF_BIN, and
# $VMAF_TINY_AI_CACHE if set; falls back to the documented defaults.
#
# Usage:
#   bash ai/scripts/run_training.sh [--epochs N] [--model-arch ARCH] ...
#
# All extra arguments are forwarded verbatim to ai/train/train.py.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

data_root="${VMAF_DATA_ROOT:-$repo_root/.workingdir2/netflix}"
out_dir="${VMAF_TRAIN_OUT_DIR:-$repo_root/runs/tiny_nflx}"
vmaf_bin="${VMAF_BIN:-$repo_root/core/build-cpu/tools/vmaf}"

if [[ ! -d "$data_root" ]]; then
  echo "error: data root not found: $data_root" >&2
  echo "Either populate \$VMAF_DATA_ROOT or copy the corpus to" \
    ".workingdir2/netflix/{ref,dis}/." >&2
  exit 2
fi

if [[ ! -x "$vmaf_bin" ]]; then
  echo "error: libvmaf CLI not found at $vmaf_bin" >&2
  echo "Build it first: meson setup core/build-cpu && ninja -C core/build-cpu" >&2
  exit 2
fi

export VMAF_BIN="$vmaf_bin"

mkdir -p "$out_dir"

python "$repo_root/ai/train/train.py" \
  --data-root "$data_root" \
  --out-dir "$out_dir" \
  "$@"
