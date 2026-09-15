#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Compatibility entrypoint for the shared release-tag replay implementation.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$ROOT/scripts/ci/ffmpeg_patch_stack.py" --check "$@"
