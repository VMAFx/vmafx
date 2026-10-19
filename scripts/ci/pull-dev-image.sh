#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# pull-dev-image.sh - log in to ghcr.io and pull the digest-pinned dev
# container, retrying a bounded number of times (issue #2792).
#
# Usage: REGISTRY_TOKEN=... GITHUB_ACTOR=... pull-dev-image.sh IMAGE
#
# A hosted runner's resolver sometimes times out on ghcr.io once; one such
# timeout failed a whole nightly tidy lane. Login and pull run under a
# timeout each and are retried up to PULL_ATTEMPTS times with growing waits.
# After the last attempt the script fails with an error naming the image.
# PULL_DOCKER and PULL_SLEEP name the docker and sleep commands; tests
# substitute stand-ins.
set -euo pipefail

readonly DOCKER="${PULL_DOCKER:-docker}"
readonly SLEEP="${PULL_SLEEP:-sleep}"
readonly ATTEMPTS="${PULL_ATTEMPTS:-4}"
readonly LOGIN_TIMEOUT=120
readonly PULL_TIMEOUT=3600
readonly DELAYS=(15 30 60)

[ "$#" -eq 1 ] || {
  echo "usage: pull-dev-image.sh IMAGE" >&2
  exit 2
}
image="$1"
case "$image" in
  *@sha256:*) ;;
  *)
    echo "::error::the dev container must be pinned by digest, got '$image'"
    exit 1
    ;;
esac

attempt_once() {
  printf '%s' "${REGISTRY_TOKEN:?REGISTRY_TOKEN is not set}" |
    timeout "$LOGIN_TIMEOUT" "$DOCKER" login ghcr.io -u "${GITHUB_ACTOR:?GITHUB_ACTOR is not set}" \
      --password-stdin &&
    timeout "$PULL_TIMEOUT" "$DOCKER" pull --quiet "$image"
}

for ((n = 1; n <= ATTEMPTS; n++)); do
  if attempt_once; then
    exit 0
  fi
  if [ "$n" -lt "$ATTEMPTS" ]; then
    i=$((n - 1))
    [ "$i" -lt "${#DELAYS[@]}" ] || i=$((${#DELAYS[@]} - 1))
    echo "pull-dev-image: attempt $n of $ATTEMPTS failed; retrying in ${DELAYS[$i]}s" >&2
    "$SLEEP" "${DELAYS[$i]}"
  fi
done
echo "::error::could not pull the dev container '$image' after $ATTEMPTS attempts"
exit 1
