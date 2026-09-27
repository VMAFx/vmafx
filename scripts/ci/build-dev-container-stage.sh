#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# build-dev-container-stage.sh — build dev/Containerfile up to its
# `libvmaf-build` stage and tag the result in the local Docker image store.
#
# One implementation, two callers:
#
#   .github/workflows/dev-container-build.yml  PR gate (ADR-0819): builds the
#       stage for every container-affecting change and smoke-tests it.
#   .github/workflows/supply-chain.yml          `build-artifacts` (ADR-1346):
#       builds the stage from the release tag's own Containerfile on a
#       GitHub-hosted runner, then runs the native release build inside it.
#
# Keeping both on this script means the PR gate exercises exactly the image
# build the release job depends on.
#
# The build is deliberately cache-free and push-free: the default Docker
# builder, no `--cache-from`/`--cache-to`, no registry. A release build must
# not restore layers another workflow wrote (ADR-1346 § Decision), and the PR
# gate's 28-35 minute builds show the uncached stage fits a hosted runner.
# Base images and SDK versions come from the Containerfile's own pinned ARG
# defaults, which scripts/ci/check-base-image-single-source.sh keeps equal to
# build-config.env; no `--build-arg` is passed, so the checked-out tree alone
# decides the inputs.
#
# GITHUB_TOKEN, when set, reaches only the Intel NEO release lookup as the
# optional BuildKit secret `github_token` (never a build argument); see
# scripts/ci/check-dev-container-build-secret.py. Unset, the build falls back
# to anonymous GitHub API access.
#
# Usage:
#   bash scripts/ci/build-dev-container-stage.sh <image-tag>
#
# Exit codes: 0 built, 2 bad invocation, otherwise docker's status.

set -euo pipefail

usage() {
  echo "usage: build-dev-container-stage.sh <image-tag>" >&2
}

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
  usage
  exit 2
fi

image_tag="$1"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

docker build \
  --file "${repo_root}/dev/Containerfile" \
  --target libvmaf-build \
  --tag "$image_tag" \
  --secret id=github_token,env=GITHUB_TOKEN \
  "$repo_root"
