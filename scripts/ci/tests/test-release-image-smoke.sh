#!/usr/bin/env bash
# Regression tests for scripts/ci/release-image-smoke.sh and
# scripts/ci/release-dry-run-plan.sh.
#
# The smoke script runs against a stub `docker` on PATH that prints what a
# healthy or a broken image would; each defect is planted on its own and must
# be refused. The planner is fed path lists and must switch on exactly the
# groups they touch. No Docker, no network.
#
# Usage: bash scripts/ci/tests/test-release-image-smoke.sh
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

if ! command -v jq >/dev/null; then
  printf 'ERROR: jq is required to run these tests\n' >&2
  exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SMOKE="$SCRIPT_DIR/../release-image-smoke.sh"
PLAN="$SCRIPT_DIR/../release-dry-run-plan.sh"
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
cat >"$scratch/bin/docker" <<'STUB'
#!/usr/bin/env bash
# Stub of `docker run`: SHIM_VERSION is what --version prints (SHIM_VERSION_RC
# its exit status); SHIM_SCORE is ok | nomean | fail for a scoring run.
args="$*"
if [[ "$args" == *"--reference"* ]]; then
  case "${SHIM_SCORE:-ok}" in
    ok) printf '{"pooled_metrics":{"vmaf":{"mean":93.2}}}\n' ;;
    nomean) printf '{"pooled_metrics":{}}\n' ;;
    fail) exit 3 ;;
  esac
  exit 0
fi
if [[ -n "${SHIM_FAIL_ENTRY:-}" && "$args" == *"$SHIM_FAIL_ENTRY"* ]]; then
  exit 4
fi
if [[ "$args" == *"--version"* || "$args" == *"-version"* || "$args" == *" version"* || "$args" == *"--help"* ]]; then
  printf '%s\n' "${SHIM_VERSION:-1.0.0-rc.2}"
  exit "${SHIM_VERSION_RC:-0}"
fi
exit 9
STUB
chmod +x "$scratch/bin/docker"

# smoke EXPECTED_RC DESCRIPTION KIND VERSION [ENV=VALUE...]
smoke() {
  local want="$1" description="$2" kind="$3" version="$4" rc=0
  shift 4
  env PATH="$scratch/bin:$PATH" "$@" "$SMOKE" local/image:ci "$kind" "$version" \
    >"$scratch/out" 2>"$scratch/err" || rc=$?
  if [[ "$rc" -eq "$want" ]]; then ok "$description"; else bad "$description (exit $rc, wanted $want: $(cat "$scratch/err"))"; fi
}

smoke 0 "accepts a healthy vmaf image" vmaf 1.0.0-rc.2
smoke 0 "accepts a healthy wrapped image" wrapped 1.0.0-rc.2
smoke 0 "accepts a healthy binary image" binary 1.0.0-rc.2
smoke 0 "accepts a healthy node image" node 1.0.0-rc.2
smoke 0 "accepts a healthy mcp image" mcp ignored
smoke 1 "refuses a node image without rclone" node 1.0.0-rc.2 SHIM_FAIL_ENTRY=/usr/local/bin/rclone
smoke 1 "refuses a node image without ffmpeg" node 1.0.0-rc.2 SHIM_FAIL_ENTRY=/usr/local/bin/ffmpeg
smoke 1 "refuses a node image that prints another version" node 1.0.0-rc.2 SHIM_VERSION=dev
smoke 1 "refuses an mcp image without its Python" mcp ignored SHIM_FAIL_ENTRY=/venv/bin/python
smoke 1 "refuses an mcp image without vmaf-mcp" mcp ignored SHIM_FAIL_ENTRY=/venv/bin/vmaf-mcp
smoke 1 "refuses an mcp image that cannot score" mcp ignored SHIM_SCORE=fail
smoke 1 "refuses a wrapped image without its vmaf binary" wrapped 1.0.0-rc.2 SHIM_FAIL_ENTRY=/usr/local/bin/vmaf
smoke 1 "refuses a vmaf image whose --version fails" vmaf 1.0.0-rc.2 SHIM_VERSION_RC=1
smoke 1 "refuses a vmaf image that cannot score" vmaf 1.0.0-rc.2 SHIM_SCORE=fail
smoke 1 "refuses a vmaf image whose score has no mean (no default model)" vmaf 1.0.0-rc.2 SHIM_SCORE=nomean
smoke 1 "refuses a wrapped image that prints another version" wrapped 1.0.0-rc.2 SHIM_VERSION=dev
smoke 1 "refuses a wrapped image that cannot score" wrapped 1.0.0-rc.2 SHIM_SCORE=fail
smoke 1 "refuses a binary image that prints another version" binary 1.0.0-rc.2 SHIM_VERSION=unknown
smoke 1 "refuses a binary image whose --version fails" binary 1.0.0-rc.2 SHIM_VERSION_RC=2

rc=0
"$SMOKE" local/image:ci bogus 1 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 ]]; then ok "exits 64 on an unknown kind"; else bad "unknown kind exited $rc"; fi
rc=0
"$SMOKE" local/image:ci >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 ]]; then ok "exits 64 on a wrong argument count"; else bad "wrong argument count exited $rc"; fi

# plan EXPECTED DESCRIPTION PATHS...: EXPECTED is "images gpu mcp" as true/false.
plan() {
  local want="$1" description="$2" got
  shift 2
  got="$(printf '%s\n' "$@" | "$PLAN" | tr '\n' ' ' | sed 's/ $//')"
  if [[ "$got" == "$want" ]]; then ok "$description"; else bad "$description (got '$got', wanted '$want')"; fi
}

none="images=false gpu=false mcp=false wheels=false"
plan "$none" "a docs-only change selects nothing" docs/development/ci.md README.md
plan "$none" "a core change selects nothing" core/src/feature/x.c
plan "images=true gpu=false mcp=false wheels=false" "the production Dockerfile selects the images" docker/Dockerfile.production
plan "images=true gpu=false mcp=false wheels=false" "the go-server Dockerfile selects the images" Dockerfile.go-server
plan "images=true gpu=false mcp=false wheels=false" "a ffmpeg patch selects the images" ffmpeg-patches/0001-x.patch
plan "images=false gpu=true mcp=false wheels=false" "the GPU Dockerfile selects the GPU images" docker/Dockerfile.production-gpu
plan "images=true gpu=true mcp=false wheels=false" "build-config.env selects images and GPU images" build-config.env
plan "images=true gpu=true mcp=false wheels=false" "the licence inputs select images and GPU images" tools/rc1-tester/image/licensing.json
plan "images=false gpu=false mcp=true wheels=false" "the vmaf-mcp package selects the mcp dry run" mcp-server/vmaf-mcp/pyproject.toml
plan "images=false gpu=false mcp=false wheels=true" "the bindings/python package selects the wheels dry run" bindings/python/pyproject.toml
plan "images=false gpu=false mcp=false wheels=true" "a core build file selects the wheels dry run" core/src/meson.build
plan "images=false gpu=false mcp=false wheels=true" "a public header selects the wheels dry run" core/include/libvmaf/libvmaf.h
plan "images=false gpu=false mcp=false wheels=false" "a core test file selects nothing" core/test/test_x.c
plan "images=false gpu=false mcp=true wheels=true" "the supply-chain workflow selects the mcp and wheels dry run" .github/workflows/supply-chain.yml
plan "images=false gpu=false mcp=true wheels=false" "the SBOM verifier selects the mcp dry run" scripts/release/verify-mcp-sbom.sh
plan "images=true gpu=true mcp=true wheels=true" "the dry run's own workflow selects everything" .github/workflows/release-dry-run.yml
plan "images=true gpu=false mcp=true wheels=false" "mixed paths combine" docker/Dockerfile.node mcp-server/vmaf-mcp/src/a.py docs/x.md
all="$("$PLAN" --all | tr '\n' ' ' | sed 's/ $//')"
if [[ "$all" == "images=true gpu=true mcp=true wheels=true" ]]; then ok "--all selects everything"; else bad "--all gave '$all'"; fi
empty="$("$PLAN" </dev/null | tr '\n' ' ' | sed 's/ $//')"
if [[ "$empty" == "$none" ]]; then ok "an empty path list selects nothing"; else bad "empty list gave '$empty'"; fi
rc=0
"$PLAN" --bogus </dev/null >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 ]]; then ok "the planner exits 64 on a bad argument"; else bad "planner bad argument exited $rc"; fi

printf '%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
