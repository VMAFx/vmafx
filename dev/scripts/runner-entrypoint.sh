#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# runner-entrypoint.sh — entrypoint for self-hosted containerised SYCL Arc runner.
# ADR-1177 / docs/development/ci-self-hosted-sycl.md

set -euo pipefail

# If custom command passed (e.g. `sycl-ls`, `./run.sh --help`, `bash`), execute directly
if [ "$#" -gt 0 ] && [ "$1" != "run-runner" ]; then
  exec "$@"
fi

if [ -z "${RUNNER_TOKEN:-}" ]; then
  echo "ERROR: RUNNER_TOKEN environment variable is not set." >&2
  echo "Generate a registration token via: gh api -X POST repos/VMAFx/vmafx/actions/runners/registration-token --jq .token" >&2
  echo "Or run arbitrary commands, e.g.: docker run --rm <image> ./run.sh --help" >&2
  exit 1
fi

REPO_URL="https://github.com/${GITHUB_REPOSITORY:-VMAFx/vmafx}"
RUNNER_NAME="${RUNNER_NAME:-$(hostname)-sycl-arc}"
RUNNER_LABELS="${RUNNER_LABELS:-self-hosted,linux,x64,sycl-arc}"
RUNNER_WORKDIR="${RUNNER_WORKDIR:-/actions-runner/_work}"

echo "Configuring ephemeral runner: name=${RUNNER_NAME}, url=${REPO_URL}, labels=${RUNNER_LABELS}"

# Configure runner in ephemeral mode (unregisters after completing one job)
./config.sh \
  --url "${REPO_URL}" \
  --token "${RUNNER_TOKEN}" \
  --name "${RUNNER_NAME}" \
  --labels "${RUNNER_LABELS}" \
  --work "${RUNNER_WORKDIR}" \
  --unattended \
  --ephemeral \
  --replace

echo "Starting runner listener..."
exec ./run.sh
