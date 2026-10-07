#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# scripts/ci/pinned-tool.sh — print the path of a release binary pinned in
# build-config.env, downloading it once into
# ${XDG_CACHE_HOME:-$HOME/.cache}/vmafx-tools/ and checking its sha256 on every
# call (ADR-2349).
#
# Usage: scripts/ci/pinned-tool.sh dashboard-linter|promtool
#
# An override variable (DASHBOARD_LINTER, PROMTOOL) names a binary to use
# instead; the script then says so on stderr. The pinned releases are
# linux_amd64; elsewhere set the override.
# Exit: 0 path printed, 2 the tool is unavailable.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
pin() { sed -n "s/^$1=\"\\(.*\\)\"\$/\\1/p" "$root/build-config.env"; }

tool="${1:-}"
case "$tool" in
  dashboard-linter)
    override="${DASHBOARD_LINTER:-}"
    version="$(pin DASHBOARD_LINTER_VERSION)"
    sha="$(pin DASHBOARD_LINTER_LINUX_AMD64_SHA256)"
    url="https://github.com/grafana/dashboard-linter/releases/download/$version/dashboard-linter_${version#v}_linux_amd64.tar.gz"
    member="dashboard-linter"
    ;;
  promtool)
    override="${PROMTOOL:-}"
    version="$(pin PROMETHEUS_VERSION)"
    sha="$(pin PROMETHEUS_LINUX_AMD64_SHA256)"
    url="https://github.com/prometheus/prometheus/releases/download/$version/prometheus-${version#v}.linux-amd64.tar.gz"
    member="prometheus-${version#v}.linux-amd64/promtool"
    ;;
  *)
    echo "pinned-tool: unknown tool '$tool' (dashboard-linter, promtool)" >&2
    exit 2
    ;;
esac

if [[ -n "$override" ]]; then
  echo "pinned-tool: using $override, not the pinned $tool $version release" >&2
  echo "$override"
  exit 0
fi
if [[ -z "$version" || -z "$sha" ]]; then
  echo "pinned-tool: the $tool version or sha256 is missing from build-config.env" >&2
  exit 2
fi
if [[ "$(uname -s)-$(uname -m)" != "Linux-x86_64" ]]; then
  echo "pinned-tool: the pinned $tool release is linux_amd64; set its override variable to a $version binary" >&2
  exit 2
fi

dir="${XDG_CACHE_HOME:-$HOME/.cache}/vmafx-tools/$tool/$version"
tarball="$dir/$tool.tar.gz"
mkdir -p "$dir"
if [[ ! -f "$tarball" ]]; then
  curl -sSfL --retry 3 --max-time 300 -o "$tarball.part" "$url"
  mv "$tarball.part" "$tarball"
fi
if ! echo "$sha  $tarball" | sha256sum --check --status; then
  echo "pinned-tool: $tarball does not match the pinned sha256; removed" >&2
  rm -f "$tarball"
  exit 2
fi
tar -xzf "$tarball" -C "$dir" "$member"
echo "$dir/$member"
