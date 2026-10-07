#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Regenerate (default) or check (--check) the committed C headers of the Rust
# ABI with cbindgen (ADR-1713):
#   core/src/rust/include/vmafx_rs.h          from vmafx-fex (core/src/rust/cbindgen.toml)
#   core/src/rust/include/vmafx_rs_predict.h  from vmafx-predict (core/src/rust/predict/cbindgen.toml),
#                                             once that configuration exists
# cbindgen is a developer tool here, not a build dependency: the build stays
# offline. Without cbindgen, --check skips with the reason and exit 0 unless
# VMAFX_REQUIRE_CBINDGEN=1 (CI sets it); regeneration fails.
set -euo pipefail

root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
mode="write"
if [[ "${1:-}" == "--check" ]]; then
  mode="check"
elif [[ $# -gt 0 ]]; then
  echo "usage: $0 [--check]" >&2
  exit 2
fi

# crate | crate directory | cbindgen configuration | generated header
specs=(
  "vmafx-fex|core/src/rust/fex|core/src/rust/cbindgen.toml|core/src/rust/include/vmafx_rs.h"
  "vmafx-predict|core/src/rust/predict|core/src/rust/predict/cbindgen.toml|core/src/rust/include/vmafx_rs_predict.h"
)

if ! command -v cbindgen >/dev/null 2>&1; then
  if [[ "$mode" == "check" && "${VMAFX_REQUIRE_CBINDGEN:-0}" != "1" ]]; then
    echo "rust-abi-header: SKIP: cbindgen not in PATH; the layout test test_rust_abi_layout still guards the ABI" >&2
    exit 0
  fi
  echo "rust-abi-header: cbindgen not in PATH (cargo install cbindgen --version 0.29.4 --locked)" >&2
  exit 2
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
status=0
for spec in "${specs[@]}"; do
  IFS='|' read -r crate dir config header <<<"$spec"
  if [[ ! -f "$root/$config" ]]; then
    echo "rust-abi-header: $crate has no $config yet; no header"
    continue
  fi
  (cd "$root" && cbindgen --quiet --config "$config" --crate "$crate" --output "$tmp" "$dir")
  if [[ "$mode" == "check" ]]; then
    if diff -u "$root/$header" "$tmp"; then
      echo "rust-abi-header: $header up to date"
    else
      echo "rust-abi-header: $header is stale; run scripts/dev/rust-abi-header.sh" >&2
      status=1
    fi
  else
    mkdir -p "$(dirname "$root/$header")"
    cp "$tmp" "$root/$header"
    echo "rust-abi-header: wrote $header"
  fi
done
exit "$status"
