#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# arc-render-node.sh — print the host /dev/dri/renderD<N> node of the Intel
# Arc (vendor 0x8086) discrete GPU, refusing to guess when there is not
# exactly one. ADR-1177 / docs/development/ci-self-hosted-sycl.md §3.
#
# renderD numbers and PCI BDFs are assigned at enumeration time and change
# after a reboot / suspend-resume / GPU hotplug (docs/state.md row
# T-DEV-CONTAINER-DRI-BIND-RACE-2026-05-18), so the compose file takes the
# node from $ARC_RENDER_NODE instead of hard-coding it:
#
#   ARC_RENDER_NODE=$(dev/scripts/arc-render-node.sh) \
#     docker compose -f dev/docker-compose.runner.yml up -d
#
# Optional: ARC_DEVICE_ID=0x56a5 pins the PCI device id as well (A380).

set -euo pipefail

want_vendor="0x8086"
want_device="${ARC_DEVICE_ID:-}"
found=()

for dev in /sys/class/drm/renderD*; do
  [[ -e "$dev/device/vendor" ]] || continue
  vendor="$(<"$dev/device/vendor")"
  device="$(<"$dev/device/device")"
  [[ "$vendor" == "$want_vendor" ]] || continue
  if [[ -n "$want_device" && "$device" != "$want_device" ]]; then
    continue
  fi
  found+=("/dev/dri/$(basename "$dev")")
done

if [[ "${#found[@]}" -ne 1 ]]; then
  echo "ERROR: expected exactly one Intel render node (vendor ${want_vendor}${want_device:+, device ${want_device}}), found ${#found[@]}: ${found[*]:-none}" >&2
  echo "       ls -l /dev/dri/by-path/ ; cat /sys/class/drm/renderD*/device/vendor" >&2
  exit 1
fi

printf '%s\n' "${found[0]}"
