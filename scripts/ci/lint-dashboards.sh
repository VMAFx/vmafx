#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# scripts/ci/lint-dashboards.sh — run Grafana's dashboard-linter (--strict, no
# exclusions) over every generated dashboard (ADR-2349).
#
# The linter is the release binary pinned in build-config.env
# (DASHBOARD_LINTER_VERSION, DASHBOARD_LINTER_LINUX_AMD64_SHA256), fetched and
# sha256-checked by scripts/ci/pinned-tool.sh. DASHBOARD_LINTER=<path> uses a
# given binary instead (other platforms, offline hosts); the run then says so.
#
# Usage: scripts/ci/lint-dashboards.sh [dashboard.json ...]
#        (default: deploy/grafana/dashboards/*.json)
# Exit: 0 every dashboard clean, 1 a lint finding, 2 the linter could not run.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
linter="$("$root/scripts/ci/pinned-tool.sh" dashboard-linter)" || exit 2

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
