#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# scripts/ci/check-prometheus-rules.sh — check the generated Prometheus rule
# file and run its unit tests with promtool (ADR-2349): `promtool check rules`
# on deploy/prometheus/vmafx-rules.yaml, then `promtool test rules` on
# deploy/prometheus/vmafx-rules.test.yaml, which holds a firing and a
# non-firing case for every alert.
#
# promtool is the release pinned in build-config.env (PROMETHEUS_VERSION,
# PROMETHEUS_LINUX_AMD64_SHA256), fetched and sha256-checked by
# scripts/ci/pinned-tool.sh; PROMTOOL=<path> uses another binary.
#
# Exit: 0 both pass, 1 a check or a test fails, 2 promtool could not run.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
promtool="$("$root/scripts/ci/pinned-tool.sh" promtool)" || exit 2
rules="$root/deploy/prometheus/vmafx-rules.yaml"
tests="$root/deploy/prometheus/vmafx-rules.test.yaml"
for f in "$rules" "$tests"; do
  if [[ ! -f "$f" ]]; then
    echo "check-prometheus-rules: $f is missing; run go run ./tools/obsgen -write" >&2
    exit 2
  fi
done

status=0
"$promtool" check rules "$rules" || status=1
# promtool resolves rule_files relative to the test file's directory.
(cd "$(dirname "$tests")" && "$promtool" test rules "$(basename "$tests")") || status=1
exit "$status"
