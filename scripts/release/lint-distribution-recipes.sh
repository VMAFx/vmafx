#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Generate the channel recipes from packaging/manifest.toml and run each
# channel's own linter on them (docs/development/distribution.md):
#   Homebrew   brew style + brew audit --strict (offline, in a throwaway tap)
#   conda      conda-smithy recipe-lint --conda-forge (staged-recipes layout)
#   winget     the vendored 1.12.0 JSON schemas (scripts/release/tests/)
# The images are digest-pinned; the containers get no network except the
# conda one, which installs the pinned conda-smithy first.
#
# Usage: scripts/release/lint-distribution-recipes.sh [--keep DIR]
# PYTHON names the interpreter (needs PyYAML; default python3).
set -euo pipefail

BREW_IMAGE="ghcr.io/homebrew/brew:latest@sha256:00b643d72ab0ee27c64e3e552d08c66e1bb5272bdf4ed28964e27aa5764f2503"
CONDA_IMAGE="condaforge/miniforge3:latest@sha256:f752860f77bd417aa4db35be3d2e906fdb740fb80f40755ed651fff9e8873aad"
CONDA_SMITHY_VERSION="3.62.0"

root="$(git rev-parse --show-toplevel)"
py="${PYTHON:-python3}"
out=""
if [[ "${1:-}" == "--keep" && -n "${2:-}" ]]; then
  out="$2"
  mkdir -p "$out"
else
  out="$(mktemp -d)"
  trap 'rm -rf "$out"' EXIT
fi

# Placeholder digests: the linters judge the recipe's form, not a download.
"$py" "$root/scripts/release/distribution.py" --root "$root" check
"$py" "$root/scripts/release/distribution.py" --root "$root" generate \
  --version 1.0.0 \
  --source-sha256 "$(printf '%064d' 0)" \
  --winget-sha256 "$(printf '%064d' 1)" \
  --out "$out"

echo "== Homebrew: brew style + brew audit --strict"
docker run --rm --network=none -v "$out/homebrew:/f:ro" "$BREW_IMAGE" bash -euc '
    export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_FROM_API=1 HOMEBREW_NO_ANALYTICS=1
    brew tap-new --no-git vmafx/lint >/dev/null
    cp /f/vmafx.rb "$(brew --repo vmafx/lint)/Formula/"
    brew style vmafx/lint/vmafx
    brew audit --strict --formula vmafx/lint/vmafx'

echo "== conda-forge: conda-smithy ${CONDA_SMITHY_VERSION} recipe-lint --conda-forge"
docker run --rm -v "$out/conda-forge:/r:ro" "$CONDA_IMAGE" bash -euc "
    # The solver's first download fails now and then; three tries, then the
    # missing conda-smithy fails the run.
    for try in 1 2 3; do
        conda install -q -y -c conda-forge --override-channels \
            conda-smithy=${CONDA_SMITHY_VERSION} >/dev/null 2>&1 && break
        sleep 10
    done
    mkdir -p /s/recipes/vmafx && cp /r/* /s/recipes/vmafx/
    conda-smithy recipe-lint --conda-forge /s/recipes/vmafx"
echo "distribution recipes: every linter passed"
