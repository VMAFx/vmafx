#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris

# Keep the stdio service's CLI contract healthy, and when Docker exposes an
# NVIDIA device, wait for the driver rather than accepting the device node as
# proof that CUDA is ready. The optional path makes the branch hermetic in the
# contract test; Compose intentionally uses the default.

set -euo pipefail

readonly NVIDIA_DEVICE="${1:-/dev/nvidia0}"

vmaf --version >/dev/null
if [[ -e "${NVIDIA_DEVICE}" ]]; then
  nvidia-smi --query-gpu=index --format=csv,noheader >/dev/null
fi
