#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install an apt package list on a GitHub runner, reusing the .deb archive
# that actions/cache restores under $RUNNER_TEMP/apt-archives.
#
#   apt-install-cached.sh configure
#       write the retry/timeout apt config only (for steps that add a repo
#       before installing)
#   apt-install-cached.sh install LIST [APT_ARGS...]
#       configure, apt-get update, restore the archive cache, install every
#       non-comment line of LIST, and copy the archives back for the save step
set -euo pipefail

usage() {
  echo "usage: $0 configure | install LIST [APT_ARGS...]" >&2
  exit 64
}

write_apt_config() {
  printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\nAPT::Keep-Downloaded-Packages "true";\n' |
    sudo tee /etc/apt/apt.conf.d/80ci-retries >/dev/null
}

install_list() {
  local list="$1"
  shift
  if [ ! -f "$list" ]; then
    echo "apt-install-cached: no package list: $list" >&2
    exit 66
  fi
  local cache="${RUNNER_TEMP:?RUNNER_TEMP is not set}/apt-archives"
  local archives="${APT_ARCHIVE_DIR:-/var/cache/apt/archives}"
  write_apt_config
  sudo apt-get update
  mkdir -p "$cache"
  if compgen -G "$cache/*.deb" >/dev/null; then
    sudo cp -f "$cache"/*.deb "$archives/"
  fi
  grep -v '^#' "$list" | xargs sudo apt-get install -y "$@"
  if compgen -G "$archives/*.deb" >/dev/null; then
    sudo cp -f "$archives"/*.deb "$cache/"
    sudo chown -R "$(id -u):$(id -g)" "$cache"
  fi
}

if [ $# -lt 1 ]; then
  usage
fi
case "$1" in
  configure)
    [ $# -eq 1 ] || usage
    write_apt_config
    ;;
  install)
    [ $# -ge 2 ] || usage
    shift
    install_list "$@"
    ;;
  *)
    usage
    ;;
esac
