#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# scripts/ci/lint-dashboards.sh — run Grafana's dashboard-linter (--strict, no
# exclusions) over every generated dashboard (ADR-2349).
#
# The linter is the release binary pinned in build-config.env
# (DASHBOARD_LINTER_VERSION, DASHBOARD_LINTER_LINUX_AMD64_SHA256), downloaded
# once into ${XDG_CACHE_HOME:-$HOME/.cache}/vmafx-tools/ and verified by its
# sha256 before every run. DASHBOARD_LINTER=<path> uses a given binary instead
# (other platforms, offline hosts); the run then says so.
#
# Usage: scripts/ci/lint-dashboards.sh [dashboard.json ...]
#        (default: deploy/grafana/dashboards/*.json)
# Exit: 0 every dashboard clean, 1 a lint finding, 2 the linter could not run.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
pin() { sed -n "s/^$1=\"\\(.*\\)\"\$/\\1/p" "$root/build-config.env"; }
version="$(pin DASHBOARD_LINTER_VERSION)"
sha="$(pin DASHBOARD_LINTER_LINUX_AMD64_SHA256)"
if [[ -z "$version" || -z "$sha" ]]; then
  echo "lint-dashboards: DASHBOARD_LINTER_VERSION / _SHA256 missing from build-config.env" >&2
  exit 2
fi

fetch_linter() {
  local dir="${XDG_CACHE_HOME:-$HOME/.cache}/vmafx-tools/dashboard-linter/$version"
  local tarball="$dir/dashboard-linter_linux_amd64.tar.gz"
  if [[ "$(uname -s)-$(uname -m)" != "Linux-x86_64" ]]; then
    echo "lint-dashboards: the pinned release is linux_amd64; set DASHBOARD_LINTER to a dashboard-linter $version binary" >&2
    return 2
  fi
  mkdir -p "$dir"
  if [[ ! -f "$tarball" ]]; then
    curl -sSfL --retry 3 --max-time 120 -o "$tarball.part" \
      "https://github.com/grafana/dashboard-linter/releases/download/$version/dashboard-linter_${version#v}_linux_amd64.tar.gz"
    mv "$tarball.part" "$tarball"
  fi
  if ! echo "$sha  $tarball" | sha256sum --check --status; then
    echo "lint-dashboards: $tarball does not match DASHBOARD_LINTER_LINUX_AMD64_SHA256; removed" >&2
    rm -f "$tarball"
    return 2
  fi
  tar -xzf "$tarball" -C "$dir" dashboard-linter
  echo "$dir/dashboard-linter"
}

if [[ -n "${DASHBOARD_LINTER:-}" ]]; then
  linter="$DASHBOARD_LINTER"
  echo "lint-dashboards: using DASHBOARD_LINTER=$linter, not the pinned $version release" >&2
else
  linter="$(fetch_linter)" || exit 2
fi

if [[ $# -eq 0 ]]; then
  shopt -s nullglob
  set -- "$root"/deploy/grafana/dashboards/*.json
  if [[ $# -eq 0 ]]; then
    echo "lint-dashboards: no dashboards under deploy/grafana/dashboards" >&2
    exit 2
  fi
fi

status=0
for dashboard in "$@"; do
  if out="$("$linter" lint --strict "$dashboard" 2>&1)"; then
    echo "ok   $dashboard"
  else
    echo "FAIL $dashboard" >&2
    sed '/^Checks that/d' <<<"$out" >&2
    status=1
  fi
done
exit "$status"
