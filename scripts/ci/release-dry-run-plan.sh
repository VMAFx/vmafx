#!/usr/bin/env bash
# Decide which release dry runs a pull request needs (release-dry-run.yml).
#
# Reads changed paths, one per line, on standard input and prints three
# `name=true|false` lines for $GITHUB_OUTPUT:
#   images  the CPU / server / operator / node release images build recipe
#   gpu     the CUDA, ROCm and oneAPI release images build recipe
#   mcp     the vmaf-mcp distribution, its SBOM and the supply-chain workflow
#   wheels  the vmafx Python wheels distribution and fixture scoring
# Usage: release-dry-run-plan.sh < changed-paths
#        release-dry-run-plan.sh --all     (a scheduled or dispatched run)
#
# A path counts for a group when it is one of that group's build inputs, the
# workflow that publishes the group, or this dry run's own files. Anything else
# leaves the group off: the dry runs are minutes to hours of runner time, so a
# pull request pays only for what it can break (ADR-1595). Exit status: 0, or 64
# on a bad argument.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

if [[ $# -gt 1 || ($# -eq 1 && "$1" != "--all") ]]; then
  printf 'Usage: release-dry-run-plan.sh [--all] < changed-paths\n' >&2
  exit 64
fi

images=false
gpu=false
mcp=false
wheels=false
if [[ "${1:-}" == "--all" ]]; then
  images=true
  gpu=true
  mcp=true
  wheels=true
else
  while IFS= read -r path || [[ -n "$path" ]]; do
    case "$path" in
      docker/Dockerfile.production-gpu | scripts/ci/install-cuda-toolkit.sh | \
        scripts/ci/install-rocm-from-image.sh | scripts/ci/install-intel-ocloc.sh | \
        scripts/ci/install-intel-oneapi.sh | dev/scripts/fetch-intel-neo.py | \
        tools/rc1-tester/image/* | REUSE.toml | LICENSES/* | build-config.env | \
        .github/workflows/docker-publish-production.yml | \
        .github/workflows/release-dry-run.yml | scripts/ci/release-dry-run-plan.sh)
        gpu=true
        ;;
    esac
    case "$path" in
      docker/Dockerfile.production | docker/Dockerfile.operator | docker/Dockerfile.node | \
        Dockerfile.go-server | ffmpeg-patches/* | tools/rc1-tester/image/* | REUSE.toml | \
        LICENSES/* | build-config.env | scripts/ci/install-cuda-toolkit.sh | \
        scripts/ci/install-intel-oneapi.sh | scripts/ci/checkout-annotated-tag.sh | \
        scripts/ci/record-copied-debian-libs.sh | scripts/ci/check-msvc-clz-shim.sh | \
        scripts/ci/release-image-smoke.sh | .github/workflows/docker-publish-production.yml | \
        .github/workflows/docker-publish-operator-node.yml | \
        .github/workflows/release-dry-run.yml | scripts/ci/release-dry-run-plan.sh)
        images=true
        ;;
    esac
    case "$path" in
      mcp-server/vmaf-mcp/* | requirements/locks/package-build.txt | \
        scripts/release/verify-mcp-sbom.sh | scripts/release/pep440-version.sh | \
        .github/workflows/supply-chain.yml | .github/workflows/release-dry-run.yml | \
        scripts/ci/release-dry-run-plan.sh)
        mcp=true
        ;;
    esac
    case "$path" in
      bindings/python/* | core/* | scripts/ci/build-wheel-libvmafx.py | \
        scripts/ci/score-wheel-fixture.py | testdata/ref_576x324_48f.yuv | \
        testdata/dis_576x324_48f.yuv | requirements/locks/build.txt | \
        requirements/locks/package-build.txt | requirements/locks/twine.txt | \
        .github/workflows/supply-chain.yml | .github/workflows/release-dry-run.yml | \
        scripts/ci/release-dry-run-plan.sh)
        wheels=true
        ;;
    esac
  done
fi

printf 'images=%s\ngpu=%s\nmcp=%s\nwheels=%s\n' "$images" "$gpu" "$mcp" "$wheels"
