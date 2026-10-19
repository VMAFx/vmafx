#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# test/e2e/incus-kind-vm.sh
#
# The virtual machine a local E2E run puts its kind cluster in.
#
# kind runs the kubelet in a privileged container, and the kubelet writes
# kernel settings that are not namespaced (vm.overcommit_memory,
# kernel.panic_on_oops, kernel.panic). On a workstation they land on the host
# kernel: with overcommit enabled, a test that allocates far more than the
# machine has succeeds and fills RAM and swap. A local run therefore creates an
# incus virtual machine with its own kernel, installs Docker and kind in it,
# and runs the cluster there; the host reaches the API server at the VM's
# address (kind-cluster.sh with VMAFX_E2E_INCUS_VM set). CI runs kind directly
# on its disposable hosted runner and does not use this script.
#
# The VM gets its own profile (named after it), never the shared `default`
# profile: CPU and memory limits, a root disk on VMAFX_E2E_VM_POOL and a NIC on
# VMAFX_E2E_VM_NETWORK.
#
# Usage: test/e2e/incus-kind-vm.sh create|destroy|address
#
# Environment variables:
#   VMAFX_E2E_INCUS_VM   VM and profile name (default: vmafx-e2e-kind)
#   VMAFX_E2E_VM_CPUS    limits.cpu (default: 6)
#   VMAFX_E2E_VM_MEMORY  limits.memory (default: 12GiB)
#   VMAFX_E2E_VM_DISK    root disk size (default: 20GiB)
#   VMAFX_E2E_VM_IMAGE   incus image (default: images:debian/13)
#   VMAFX_E2E_VM_POOL    storage pool of the root disk (default: default)
#   VMAFX_E2E_VM_NETWORK managed network of the NIC (default: incusbr0)
#   KIND_VERSION         kind release installed in the VM (default: v0.33.0,
#                        the version .github/workflows/e2e-k8s.yml pins)
#
# Prerequisites on the host: incus (member of incus-admin), KVM, curl,
# sha256sum, python3.

set -euo pipefail

VM="${VMAFX_E2E_INCUS_VM:-vmafx-e2e-kind}"
CPUS="${VMAFX_E2E_VM_CPUS:-6}"
MEMORY="${VMAFX_E2E_VM_MEMORY:-12GiB}"
DISK="${VMAFX_E2E_VM_DISK:-20GiB}"
IMAGE="${VMAFX_E2E_VM_IMAGE:-images:debian/13}"
POOL="${VMAFX_E2E_VM_POOL:-default}"
NETWORK="${VMAFX_E2E_VM_NETWORK:-incusbr0}"
KIND_VERSION="${KIND_VERSION:-v0.33.0}"
# Bounded waits: polls of 2 s.
AGENT_POLLS=90
ADDRESS_POLLS=60

log() { printf '[incus-kind-vm] %s\n' "$*" >&2; }
die() {
  printf '[incus-kind-vm] ERROR: %s\n' "$*" >&2
  exit 1
}

[[ "${VM}" =~ ^[a-z][a-z0-9-]{1,62}$ ]] || die "VM name ${VM@Q} is not a plain lower-case name"
[[ "${VM}" != default ]] || die "the VM and its profile must not be named default"

vm_exists() { incus info "${VM}" >/dev/null 2>&1; }

# address prints the IPv4 address of the VM's NIC on VMAFX_E2E_VM_NETWORK
# (the interface whose MAC is the one incus gave eth0), empty while it has none.
address() {
  incus query "/1.0/instances/${VM}?recursion=1" | python3 -c '
import json, sys
inst = json.load(sys.stdin)
mac = inst["config"].get("volatile.eth0.hwaddr", "")
for nic in ((inst.get("state") or {}).get("network") or {}).values():
    if nic.get("hwaddr") != mac:
        continue
    for addr in nic.get("addresses", []):
        if addr.get("family") == "inet" and addr.get("scope") == "global":
            print(addr["address"])
            sys.exit(0)
'
}

wait_for() {
  local polls="$1" what="$2"
  shift 2
  for _ in $(seq 1 "${polls}"); do
    if "$@" >/dev/null 2>&1; then
      return 0
    fi
    sleep 2
  done
  die "${what} not ready after $((polls * 2)) s"
}

has_address() { [[ -n "$(address)" ]]; }

# create_profile creates or updates the VM's own profile.
create_profile() {
  if incus profile show "${VM}" >/dev/null 2>&1; then
    incus profile device set "${VM}" root pool="${POOL}" size="${DISK}"
    incus profile device set "${VM}" eth0 network="${NETWORK}"
  else
    incus profile create "${VM}"
    incus profile device add "${VM}" root disk path=/ pool="${POOL}" size="${DISK}"
    incus profile device add "${VM}" eth0 nic network="${NETWORK}" name=eth0
  fi
  incus profile set "${VM}" limits.cpu="${CPUS}" limits.memory="${MEMORY}"
}

install_kind() {
  local work
  work="$(mktemp -d)"
  # shellcheck disable=SC2064 # expand now: the trap must remove this directory
  trap "rm -rf '${work}'" RETURN
  curl --fail --location --show-error --silent \
    --connect-timeout 20 --max-time 300 --retry 5 --retry-all-errors \
    --output "${work}/kind" \
    "https://kind.sigs.k8s.io/dl/${KIND_VERSION}/kind-linux-amd64"
  curl --fail --location --show-error --silent \
    --connect-timeout 20 --max-time 60 --retry 5 --retry-all-errors \
    --output "${work}/kind.sha256sum" \
    "https://kind.sigs.k8s.io/dl/${KIND_VERSION}/kind-linux-amd64.sha256sum"
  printf '%s  %s\n' "$(cut -d' ' -f1 "${work}/kind.sha256sum")" "${work}/kind" |
    sha256sum --check --strict
  incus file push --mode 0755 "${work}/kind" "${VM}/usr/local/bin/kind"
}

create() {
  if vm_exists; then
    log "VM ${VM} exists; reusing it"
    if [[ "$(incus list "${VM}" --columns s --format csv)" != RUNNING ]]; then
      incus start "${VM}"
    fi
  else
    create_profile
    log "Launching VM ${VM} (${IMAGE}, ${CPUS} CPUs, ${MEMORY})"
    incus launch "${IMAGE}" "${VM}" --vm --no-profiles --profile "${VM}"
  fi
  wait_for "${AGENT_POLLS}" "incus agent of ${VM}" incus exec "${VM}" -- true
  if ! incus exec "${VM}" -- sh -c 'command -v docker >/dev/null' 2>/dev/null; then
    log "Installing Docker in ${VM}"
    incus exec "${VM}" --env DEBIAN_FRONTEND=noninteractive -- sh -euc \
      'apt-get update -q && apt-get install -y -q --no-install-recommends docker.io docker-cli ca-certificates'
  fi
  wait_for "${AGENT_POLLS}" "Docker in ${VM}" incus exec "${VM}" -- docker info --format '{{.ServerVersion}}'
  if ! incus exec "${VM}" -- sh -c "kind version | grep -q '^kind ${KIND_VERSION} '" 2>/dev/null; then
    log "Installing kind ${KIND_VERSION} in ${VM}"
    install_kind
  fi
  wait_for "${ADDRESS_POLLS}" "IPv4 address of ${VM}" has_address
  log "VM ${VM} ready at $(address)"
}

destroy() {
  if vm_exists; then
    log "Deleting VM ${VM}"
    incus delete --force "${VM}"
  fi
  if incus profile show "${VM}" >/dev/null 2>&1; then
    incus profile delete "${VM}"
  fi
}

case "${1:-}" in
  create) create ;;
  destroy) destroy ;;
  address)
    ip="$(address)"
    [[ -n "${ip}" ]] || die "VM ${VM} has no IPv4 address on ${NETWORK}"
    printf '%s\n' "${ip}"
    ;;
  *) die "usage: $0 create|destroy|address" ;;
esac
