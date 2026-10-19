#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# scripts/ci/observability-compose-smoke.sh — start the Compose observability
# example (deploy/compose/observability) and run its smoke test
# (tools/obssmoke): every component scraped, the rendered rules healthy,
# every dashboard query returning data, Grafana provisioned, traces in Tempo
# (ADR-2349, ADR-2399).
#
# Usage: scripts/ci/observability-compose-smoke.sh [--build] [--keep]
#   --build  build the VMAFx images from this checkout first (docker compose
#            build; the node image builds FFmpeg, which takes a while)
#   --keep   leave the stack running afterwards (default: docker compose down -v)
#
# VMAFX_IMAGE_REGISTRY and VMAFX_TAG name the images (default local/...:dev,
# what --build produces). The media is the 576x324 pair of testdata/, written
# as Y4M and raw into a temporary directory. Host ports are ephemeral, so the
# run does not collide with services already listening. The node is
# restarted once it has been scraped, which the restart annotation shows.
# Exit: 0 every check passed, 1 a check failed, 2 a usage or setup error.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build=false
keep=false
for arg in "$@"; do
  case "$arg" in
    --build) build=true ;;
    --keep) keep=true ;;
    *)
      echo "usage: $0 [--build] [--keep]" >&2
      exit 2
      ;;
  esac
done
command -v docker >/dev/null || {
  echo "observability-compose-smoke: docker is not on PATH" >&2
  exit 2
}

media="$(mktemp -d)"
chmod 755 "$media"
project="vmafx-obs-smoke-$$"
compose=(docker compose -p "$project" -f "$root/deploy/compose/observability/compose.yaml")
export VMAFX_MEDIA_DIR="$media"
# Test prices in XTS, the ISO 4217 code reserved for testing, so the cost
# panels have data; they are inputs of this test, not prices.
export VMAFX_MONITORING_VALUES="$media/monitoring-values.yaml"
printf 'monitoring:\n  cost: {perJobSecond: 0.001, perJob: 0.01, currency: XTS}\n' >"$VMAFX_MONITORING_VALUES"
export PROMETHEUS_PORT=0 GRAFANA_PORT=0 VMAFX_SERVER_PORT=0 VMAFX_CONTROLLER_GRPC_PORT=0

cleanup() {
  local status=$?
  if [[ "$keep" == false ]]; then
    "${compose[@]}" down -v --remove-orphans >/dev/null 2>&1 || status=1
  else
    echo "observability-compose-smoke: stack $project left running; media in $media" >&2
  fi
  [[ "$keep" == true ]] || rm -rf "$media"
  exit "$status"
}
trap cleanup EXIT

python3 "$root/scripts/ci/observability_smoke_media.py" \
  "$root/testdata/ref_576x324_48f.yuv" "$root/testdata/dis_576x324_48f.yuv" "$media"

if [[ "$build" == true ]]; then
  "${compose[@]}" build vmafx-server vmafx-controller vmafx-node
fi
"${compose[@]}" up -d --no-build

# Restart the node once Prometheus has scraped it, so the dashboards'
# "Deploys and restarts" annotation has a restart to show.
node_scraped() {
  "${compose[@]}" exec -T prometheus wget -qO- \
    'http://localhost:9090/api/v1/query?query=up%7Bjob%3D%22vmafx-node%22%7D%3D%3D1' 2>/dev/null |
    grep -q '"result":\[{'
}
for _ in $(seq 1 90); do
  node_scraped && break
  sleep 2
done
node_scraped || {
  echo "observability-compose-smoke: Prometheus never scraped vmafx-node" >&2
  "${compose[@]}" logs --no-color --tail=40 vmafx-node prometheus rules >&2 ||
    echo "observability-compose-smoke: could not read the service logs" >&2
  exit 1
}
"${compose[@]}" restart vmafx-node

if ! "${compose[@]}" --profile smoke run --rm smoke; then
  "${compose[@]}" logs --no-color --tail=80 >&2 ||
    echo "observability-compose-smoke: could not read the service logs" >&2
  exit 1
fi
