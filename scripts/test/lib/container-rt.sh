# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# shellcheck shell=bash
# Sourced helper for scripts/test/*.sh that `run` the build image.
#
# Sets:
#   REPO   - repo root as seen by this shell (/src inside the devcontainer)
#   RT     - container CLI: $CONTAINER_RT, else podman, else docker
# Defines:
#   host_path PATH - translate a path under $REPO to the path the container
#                    runtime sees. Inside the devcontainer the runtime is the
#                    HOST's podman (via the forwarded socket, CONTAINER_HOST),
#                    so `-v SRC:DST` sources must be host paths; HOST_REPO
#                    (set by .devcontainer/devcontainer.json) maps /src back
#                    to the host checkout. Outside the devcontainer it is a
#                    no-op.

REPO="$(git rev-parse --show-toplevel)"

RT="${CONTAINER_RT:-$(command -v podman || command -v docker || true)}"
if [ -z "$RT" ]; then
  echo "error: no container runtime (podman/docker) on PATH." >&2
  echo "       In the devcontainer: enable the host socket" >&2
  echo "       (systemctl --user enable --now podman.socket) and rebuild the" >&2
  echo "       devcontainer; see docs/development/ide-setup.md." >&2
  exit 127
fi

host_path() {
  local hrepo="${HOST_REPO:-$REPO}"
  case "$1" in
    "$REPO") printf '%s\n' "$hrepo" ;;
    "$REPO"/*) printf '%s\n' "$hrepo${1#"$REPO"}" ;;
    *) printf '%s\n' "$1" ;;
  esac
}
