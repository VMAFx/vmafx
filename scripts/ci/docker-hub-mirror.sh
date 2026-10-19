#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/docker-hub-mirror.sh -- route the runner's docker.io pulls through a
# pull-through mirror (default mirror.gcr.io), so hosted runners stop hitting Docker Hub's
# anonymous pull limit. The pattern is golusoris #741's: the mirror is set on the Docker
# daemon, so it also covers the testcontainers pulls inside `go test` (the PostgreSQL images
# of cmd/vmafx-controller/store/storetest). Pinned digests stay valid: the mirror serves the
# same content addresses.
#
# Usage:
#   docker-hub-mirror.sh                 configure the local Docker daemon (Linux runner, sudo)
#   docker-hub-mirror.sh --print FILE    print FILE's daemon.json with the mirror merged in
#                                        (no root, no daemon; scripts/ci/tests/test_docker_hub_mirror.py)
#
# DOCKER_HUB_MIRROR overrides the mirror. Exit 0 configured or printed, 1 the daemon did not
# load the mirror or the daemon.json is malformed, 2 usage.

set -euo pipefail

mirror="${DOCKER_HUB_MIRROR:-https://mirror.gcr.io}"
readonly mirror

# merge_config reads daemon.json on stdin and keeps every existing key, adding the mirror to
# registry-mirrors once.
merge_config() {
  jq --arg m "$mirror" '.["registry-mirrors"] = ((([$m]) + (.["registry-mirrors"] // [])) | unique)'
}

if [[ "${1:-}" == --print ]]; then
  if [[ $# -ne 2 ]]; then
    printf 'usage: %s [--print FILE]\n' "$0" >&2
    exit 2
  fi
  if [[ -s "$2" ]]; then
    merge_config <"$2"
  else
    printf '{}' | merge_config
  fi
  exit 0
fi
if [[ $# -ne 0 ]]; then
  printf 'usage: %s [--print FILE]\n' "$0" >&2
  exit 2
fi

if [[ "$(uname -s)" != Linux ]]; then
  printf 'docker-hub-mirror: skip: %s runners run no Linux Docker daemon to configure\n' "$(uname -s)"
  exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  printf 'docker-hub-mirror: skip: docker is not installed on this runner\n'
  exit 0
fi

conf=/etc/docker/daemon.json
current='{}'
if sudo test -s "$conf"; then
  current="$(sudo cat "$conf")"
fi
merged="$(printf '%s' "$current" | merge_config)"
printf '%s\n' "$merged" | sudo tee "$conf" >/dev/null
sudo systemctl restart docker

# Bounded wait for the restarted daemon (HISS-02).
for _ in $(seq 1 30); do
  if docker info >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
mirrors="$(docker info --format '{{json .RegistryConfig.Mirrors}}')"
if [[ "$mirrors" != *"${mirror#https://}"* ]]; then
  printf '::error title=Docker Hub mirror::the daemon did not load %s (mirrors: %s)\n' "$mirror" "$mirrors"
  exit 1
fi
printf 'docker-hub-mirror: docker.io pulls go through %s\n' "$mirror"
