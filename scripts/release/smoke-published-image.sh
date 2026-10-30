#!/usr/bin/env bash
# Smoke-test one image that docker-publish-operator-node.yml has just
# published and whose signature the job has verified.
#
# Usage: smoke-published-image.sh IMAGE KIND VERSION
#   IMAGE    the published image reference, by digest (it is pulled)
#   KIND     operator | server | controller | node
#   VERSION  what the entrypoint's `--version` must print (the release tag)
#
# Checks the entrypoint (and the nonroot user of server and controller), then
# runs scripts/ci/release-image-smoke.sh for `--version` and default-model
# scoring (one implementation, HISS-19). On top of that:
#   server      starts the HTTP server; /healthz and /readyz answer
#   controller  starts with auth disabled (ADR-1589); /readyz answers once
#               its SQLite queue sits in the image's /data
#   node        mount mode (ADR-1593): as UID 65532, with only the bounding-set
#               capabilities node.fuse grants, the setuid fusermount3 and the
#               util-linux mount it runs mount an rclone remote; a file reads
#               back through FUSE and unmounts. AppArmor (on the runner)
#               denies mount under Docker's default profile.
#
# Exit status: 0 = every check passed, 1 = a check failed, 64 = bad usage.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
READY_TRIES="${SMOKE_READY_TRIES:-30}"

if [[ $# -ne 3 ]]; then
  printf 'Usage: smoke-published-image.sh IMAGE KIND VERSION\n' >&2
  exit 64
fi
image="$1"
kind="$2"
version="$3"
case "$kind" in
  operator | controller) smoke_kind=binary ;;
  server) smoke_kind=wrapped ;;
  node) smoke_kind=node ;;
  *)
    printf 'ERROR: unknown KIND %s (operator, server, controller or node)\n' "$kind" >&2
    exit 64
    ;;
esac

fail() {
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

container=""
cleanup() {
  local status=$?
  if [[ -n "$container" ]]; then
    if [[ "$status" -ne 0 ]] && ! docker logs "$container" 2>&1; then
      printf '::warning::no container logs for %s\n' "$container" >&2
    fi
    if docker inspect "$container" >/dev/null 2>&1; then
      docker rm --force "$container" >/dev/null
    fi
  fi
  rm -rf "${frames:-}"
}
trap cleanup EXIT

# check_config: the image's entrypoint is vmafx-KIND; server and controller
# run as the nonroot UID.
check_config() {
  local entrypoint user
  entrypoint="$(docker image inspect --format '{{json .Config.Entrypoint}}' "$image")"
  [[ "$entrypoint" == "[\"/usr/local/bin/vmafx-$kind\"]" ]] ||
    fail "$image: entrypoint is $entrypoint, expected /usr/local/bin/vmafx-$kind"
  if [[ "$kind" == server || "$kind" == controller ]]; then
    user="$(docker image inspect --format '{{.Config.User}}' "$image")"
    [[ "$user" == 65532:65532 ]] || fail "$image: runs as '$user', expected 65532:65532"
  fi
}

# wait_ready PATH...: start the image detached and wait until every PATH answers.
# The controller starts with auth disabled; the server needs no setting.
wait_ready() {
  local address path ready=false env_args=()
  [[ "$kind" == controller ]] && env_args=(--env VMAFX_AUTH_DISABLED=true)
  container="vmafx-$kind-release-smoke"
  docker run --detach --name "$container" "${env_args[@]}" \
    --publish 127.0.0.1::8080 "$image" >/dev/null
  address="$(docker port "$container" 8080/tcp)"
  for ((try = 0; try < READY_TRIES; try++)); do
    [[ "$(docker inspect --format '{{.State.Running}}' "$container")" == true ]] ||
      fail "vmafx-$kind exited before becoming ready"
    ready=true
    for path in "$@"; do
      curl --fail --silent --show-error --max-time 5 "http://$address$path" >/dev/null ||
        ready=false
    done
    [[ "$ready" == true ]] && return 0
    sleep 1
  done
  fail "vmafx-$kind did not answer $* within $READY_TRIES tries"
}

# check_mount: mount a local rclone remote through FUSE and read a file back.
check_mount() {
  local inside outside
  frames="$(mktemp -d)"
  head -c $((576 * 324 * 3)) /dev/urandom >"$frames/ref.yuv"
  container="vmafx-node-mount-smoke"
  docker run --detach --name "$container" --device /dev/fuse \
    --cap-drop ALL --cap-add SYS_ADMIN --cap-add DAC_READ_SEARCH \
    --security-opt apparmor=unconfined --read-only --tmpfs /tmp:mode=1777 \
    -v "$frames:/frames:ro" --entrypoint /usr/local/bin/rclone "$image" \
    rcd --rc-no-auth --rc-addr 127.0.0.1:5572 >/dev/null
  docker exec "$container" rclone mkdir /tmp/m || fail "$image: rclone mkdir failed"
  timeout 60 docker exec "$container" \
    rclone mount :local:/frames /tmp/m --daemon --vfs-cache-mode off ||
    fail "$image: rclone mount through FUSE failed"
  inside="$(docker exec "$container" rclone md5sum /tmp/m/ref.yuv | cut -d' ' -f1)"
  outside="$(md5sum "$frames/ref.yuv" | cut -d' ' -f1)"
  [[ "$inside" == "$outside" ]] || fail "$image: the file read through FUSE differs"
  docker exec "$container" /usr/bin/fusermount3 -u /tmp/m ||
    fail "$image: fusermount3 could not unmount /tmp/m"
}

docker pull "$image"
check_config
bash "$REPO_ROOT/scripts/ci/release-image-smoke.sh" "$image" "$smoke_kind" "$version"
case "$kind" in
  server) wait_ready /healthz /readyz ;;
  controller) wait_ready /readyz ;;
  node) check_mount ;;
  operator) ;;
esac
printf 'OK: published %s (%s)\n' "$image" "$kind"
