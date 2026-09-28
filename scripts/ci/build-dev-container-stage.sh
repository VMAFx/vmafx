#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# build-dev-container-stage.sh — build one named stage of dev/Containerfile
# and tag the result in the local Docker image store.
#
# One implementation, every CI caller that needs a stage image to run:
#
#   .github/workflows/supply-chain.yml          `build-artifacts` (ADR-1346,
#       ADR-1354): builds `release-build` from the release tag's own
#       Containerfile on a GitHub-hosted runner, then runs the native release
#       build inside it.
#   .github/workflows/dev-container-build.yml  PR gate (ADR-0819): builds
#       `libvmaf-build` and smoke-tests it, then builds `release-build` and
#       rehearses the release build in it exactly as `build-artifacts` does.
#
# Keeping every caller on this script means the PR gate exercises exactly the
# image build the release job depends on. `docker build --check` (lint only,
# no image) and dev-container-publish.yml (publishes to GHCR, never on the
# release path) are the only other builds of dev/Containerfile in workflows.
#
# The target is an explicit argument from a fixed allowlist so no caller can
# build a stage by accident:
#
#   release-build  digest-pinned Debian 13 base (RELEASE_BUILDER_BASE) plus
#                  Debian archive packages (gcc, meson, ninja, nasm, ...). No
#                  third-party download, so no GitHub token: nothing in the
#                  stage could use one.
#   libvmaf-build  adds the GPU SDKs and third-party fetches; the Intel NEO
#                  release lookup takes the optional BuildKit secret
#                  `github_token` from GITHUB_TOKEN (never a build argument).
#                  Unset, that lookup falls back to anonymous API access.
#
# scripts/ci/check-dev-container-build-secret.py derives, from the
# Containerfile's stage graph, which targets reach the `github_token` mount and
# fails unless this script forwards the secret for exactly those targets.
#
# No external layer cache and no registry push: no `--cache-from`/`--cache-to`,
# no push. The only pulls are the digest-pinned base images and BuildKit
# frontend the Containerfile names. A release build must not restore layers another workflow
# wrote (ADR-1346 § Decision). The daemon's own build cache still applies; it
# starts empty on a GitHub-hosted runner, and inside one job it lets a second
# build reuse the layers the first one just produced. Base images and SDK
# versions come from the Containerfile's own ARG defaults, which
# scripts/ci/check-base-image-single-source.sh keeps equal to build-config.env;
# no `--build-arg` is passed, so the checked-out tree alone decides the inputs.
#
# Usage:
#   bash scripts/ci/build-dev-container-stage.sh <target> <image-tag>
#
# Exit codes: 0 built, 2 bad invocation, otherwise docker's status.

set -euo pipefail

usage() {
  echo "usage: build-dev-container-stage.sh {release-build|libvmaf-build} <image-tag>" >&2
}

if [ "$#" -ne 2 ] || [ -z "$1" ] || [ -z "$2" ]; then
  usage
  exit 2
fi

target="$1"
image_tag="$2"

case "$target" in
  release-build)
    secret_args=()
    ;;
  libvmaf-build)
    secret_args=(--secret "id=github_token,env=GITHUB_TOKEN")
    ;;
  *)
    echo "build-dev-container-stage: unsupported target: ${target}" >&2
    usage
    exit 2
    ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

docker build \
  --file "${repo_root}/dev/Containerfile" \
  --target "$target" \
  --tag "$image_tag" \
  "${secret_args[@]}" \
  "$repo_root"
