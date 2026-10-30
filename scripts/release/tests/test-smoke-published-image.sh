#!/usr/bin/env bash
# Regression tests for scripts/release/smoke-published-image.sh, the smoke
# test docker-publish-operator-node.yml runs on each image it publishes.
#
# The script runs against stub `docker` and `curl` commands on PATH that
# answer like a healthy published image; each defect is planted on its own
# and must be refused. No Docker, no network.
#
# Usage: bash scripts/release/tests/test-smoke-published-image.sh
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

if ! command -v jq >/dev/null; then
  printf 'ERROR: jq is required to run these tests\n' >&2
  exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SMOKE="$SCRIPT_DIR/../smoke-published-image.sh"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0
ok() {
  pass=$((pass + 1))
  printf 'ok   %s\n' "$1"
}
bad() {
  fail=$((fail + 1))
  printf 'FAIL %s\n' "$1" >&2
}

mkdir -p "$scratch/bin"
# Stub docker. SHIM_KIND names the image's binary (vmafx-SHIM_KIND);
# SHIM_ENTRY / SHIM_USER override the inspected config; SHIM_VERSION is what
# --version prints; SHIM_EXITED=1 reports the server container stopped;
# SHIM_MD5 is what the FUSE read-back hashes to (default: the real file's);
# SHIM_FAIL_EXEC fails the `docker exec` whose arguments contain it;
# SHIM_RUN_ENV_LOG records the environment arguments of a detached run.
cat >"$scratch/bin/docker" <<'STUB'
#!/usr/bin/env bash
args="$*"
case "$1" in
  pull | rm | logs) exit 0 ;;
  port) printf '127.0.0.1:49153\n'; exit 0 ;;
  image)
    if [[ "$args" == *Entrypoint* ]]; then
      printf '%s\n' "${SHIM_ENTRY:-[\"/usr/local/bin/vmafx-$SHIM_KIND\"]}"
    else
      printf '%s\n' "${SHIM_USER-65532:65532}"
    fi
    exit 0
    ;;
  inspect)
    [[ "$args" == *State.Running* ]] || exit 0
    if [[ "${SHIM_EXITED:-0}" == 1 ]]; then printf 'false\n'; else printf 'true\n'; fi
    exit 0
    ;;
  exec)
    if [[ "$args" == *md5sum* ]]; then
      printf '%s  /tmp/m/ref.yuv\n' "${SHIM_MD5:-$(md5sum "$(cat "$SHIM_STATE/frames")/ref.yuv" | cut -d' ' -f1)}"
    elif [[ -n "${SHIM_FAIL_EXEC:-}" && "$args" == *"$SHIM_FAIL_EXEC"* ]]; then
      exit 5
    fi
    exit 0
    ;;
esac
if [[ "$args" == *--detach* ]]; then
  printf '%s\n' "$args" >>"${SHIM_RUN_ENV_LOG:-/dev/null}"
  # The mount container mounts the frames folder; remember it for md5sum.
  frames="$(sed -n 's/.*-v \([^:]*\):\/frames:ro.*/\1/p' <<<"$args")"
  [[ -n "$frames" ]] && printf '%s\n' "$frames" >"$SHIM_STATE/frames"
  exit 0
fi
if [[ "$args" == *"--reference"* ]]; then
  printf '{"pooled_metrics":{"vmaf":{"mean":93.2}}}\n'
  exit 0
fi
if [[ "$args" == *"--version"* || "$args" == *"-version"* || "$args" == *" version"* ]]; then
  printf '%s\n' "${SHIM_VERSION:-0.1.0}"
  exit 0
fi
exit 9
STUB
cat >"$scratch/bin/curl" <<'STUB'
#!/usr/bin/env bash
[[ -n "${SHIM_CURL_FAIL:-}" && "$*" == *"$SHIM_CURL_FAIL"* ]] && exit 22
exit 0
STUB
chmod +x "$scratch/bin/docker" "$scratch/bin/curl"

# smoke EXPECTED_RC DESCRIPTION KIND [ENV=VALUE...]
smoke() {
  local want="$1" description="$2" kind="$3" rc=0
  shift 3
  rm -f "$scratch/state/frames"
  mkdir -p "$scratch/state"
  env PATH="$scratch/bin:$PATH" SHIM_KIND="$kind" SHIM_STATE="$scratch/state" \
    SMOKE_READY_TRIES=2 "$@" "$SMOKE" ghcr.io/vmafx/image@sha256:0 "$kind" 0.1.0 \
    >"$scratch/out" 2>"$scratch/err" || rc=$?
  if [[ "$rc" -eq "$want" ]]; then ok "$description"; else bad "$description (exit $rc, wanted $want: $(tail -3 "$scratch/err"))"; fi
}

smoke 0 "accepts a healthy operator image" operator
smoke 0 "accepts a healthy server image" server
smoke 0 "accepts a healthy controller image" controller
smoke 0 "accepts a healthy node image" node
smoke 1 "refuses a node image whose FUSE read-back differs" node SHIM_MD5=00000000000000000000000000000000
smoke 1 "refuses a node image whose rclone mount fails" node SHIM_FAIL_EXEC="rclone mount"
smoke 1 "refuses a node image whose fusermount3 cannot unmount" node SHIM_FAIL_EXEC=fusermount3
smoke 1 "refuses a node image with another entrypoint" node SHIM_ENTRY='["/usr/local/bin/vmafx-operator"]'
smoke 1 "refuses a node image that prints another version" node SHIM_VERSION=dev
smoke 1 "refuses an operator image with another entrypoint" operator SHIM_ENTRY='["/bin/sh"]'
smoke 1 "refuses a server image that runs as root" server SHIM_USER=0:0
smoke 1 "refuses a controller image that runs as root" controller SHIM_USER=
smoke 1 "refuses a server image that prints another version" server SHIM_VERSION=dev
smoke 1 "refuses a server whose /readyz never answers" server SHIM_CURL_FAIL=/readyz
smoke 1 "refuses a server whose /healthz never answers" server SHIM_CURL_FAIL=/healthz
smoke 1 "refuses a server that exits before it is ready" server SHIM_EXITED=1
smoke 1 "refuses a controller whose /readyz never answers" controller SHIM_CURL_FAIL=/readyz

# Only the controller starts with auth disabled.
log="$scratch/run-env.log"
: >"$log"
smoke 0 "starts the server for the HTTP check" server SHIM_RUN_ENV_LOG="$log"
if grep -q VMAFX_AUTH_DISABLED "$log"; then bad "the server starts with auth disabled"; else ok "the server starts with its own auth setting"; fi
: >"$log"
smoke 0 "starts the controller for the HTTP check" controller SHIM_RUN_ENV_LOG="$log"
if grep -q 'VMAFX_AUTH_DISABLED=true' "$log"; then ok "the controller starts with auth disabled"; else bad "the controller starts without VMAFX_AUTH_DISABLED=true"; fi

rc=0
"$SMOKE" ghcr.io/vmafx/image@sha256:0 bogus 1 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 ]]; then ok "exits 64 on an unknown kind"; else bad "unknown kind exited $rc"; fi
rc=0
"$SMOKE" ghcr.io/vmafx/image@sha256:0 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 ]]; then ok "exits 64 on a wrong argument count"; else bad "wrong argument count exited $rc"; fi

printf '%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
