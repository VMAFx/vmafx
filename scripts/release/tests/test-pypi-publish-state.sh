#!/usr/bin/env bash
# Regression tests for scripts/release/pypi-publish-state.sh, the PyPI
# pre-publish divergence check of the vmafx wheel publication (#2318).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# A fake `curl` on PATH serves a fixture JSON with a chosen HTTP status, so the
# script runs unchanged and offline. Each case asserts the exit status and the
# printed decision.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
STATE="$SCRIPT_DIR/../pypi-publish-state.sh"
# The hook runs shellcheck per file without -x, so it cannot follow lib.sh,
# which defines scratch, pass, fail, ok, bad and check.
# shellcheck source=scripts/release/tests/lib.sh disable=SC1091,SC2154
. "$SCRIPT_DIR/lib.sh"

# shellcheck disable=SC2154  # scratch comes from lib.sh
bin="$scratch/bin"
mkdir -p "$bin"
cat >"$bin/curl" <<'CURL'
#!/usr/bin/env bash
# Fake curl: copy $FAKE_PYPI_JSON to the --output path, print $FAKE_PYPI_CODE.
out=""
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "--output" ]]; then out="$2"; shift; fi
  shift
done
[[ -n "${FAKE_PYPI_JSON:-}" ]] && cp -- "$FAKE_PYPI_JSON" "$out"
printf '%s' "$FAKE_PYPI_CODE"
CURL
chmod +x "$bin/curl"

make_dist() {
  local dir="$1"
  rm -rf "$dir"
  mkdir -p "$dir"
  printf 'wheel-a' >"$dir/vmafx-0.1.0-cp314-cp314-manylinux_2_28_x86_64.whl"
  printf 'wheel-b' >"$dir/vmafx-0.1.0-cp314-cp314-win_amd64.whl"
  printf 'sdist' >"$dir/vmafx-0.1.0.tar.gz"
}

# pypi_json <out> <file>... : PyPI JSON listing each file with its real digest;
# `name=digest` overrides the digest for one entry.
pypi_json() {
  local out="$1" entry name digest
  shift
  {
    printf '{"urls":['
    local first=1
    for entry in "$@"; do
      name="${entry%%=*}"
      if [[ "$entry" == *=* ]]; then
        digest="${entry#*=}"
      else
        digest="$(sha256sum -- "$dist/$name" | cut -d' ' -f1)"
      fi
      [[ $first -eq 1 ]] || printf ','
      first=0
      printf '{"filename":"%s","digests":{"sha256":"%s"}}' "$name" "$digest"
    done
    printf ']}'
  } >"$out"
}

# run_case <description> <expected rc> <expected stdout or -> <http code> [json]
run_case() {
  local description="$1" want_rc="$2" want_out="$3" code="$4" json="${5:-}"
  local out rc=0
  out="$(cd "$scratch" && PATH="$bin:$PATH" FAKE_PYPI_CODE="$code" \
    FAKE_PYPI_JSON="$json" GITHUB_OUTPUT="" \
    bash "$STATE" vmafx vmafx 0.1.0 "$dist" 2>/dev/null)" || rc=$?
  if [[ "$rc" -eq "$want_rc" && ("$want_out" == "-" || "$out" == "$want_out") ]]; then
    ok "$description"
  else
    bad "$description (rc=$rc out=$out)"
  fi
}

dist="$scratch/dist"
make_dist "$dist"
w1=vmafx-0.1.0-cp314-cp314-manylinux_2_28_x86_64.whl
w2=vmafx-0.1.0-cp314-cp314-win_amd64.whl
sd=vmafx-0.1.0.tar.gz

# Positive.
run_case "version absent on PyPI (404) owes a publish" 0 publish=true 404
pypi_json "$scratch/all.json" "$w1" "$w2" "$sd"
run_case "every file present with the same digest skips the publish" 0 publish=false 200 \
  "$scratch/all.json"
pypi_json "$scratch/part.json" "$w1"
run_case "a partially uploaded version owes the rest" 0 publish=true 200 "$scratch/part.json"

# Negative.
pypi_json "$scratch/digest.json" "$w1" "$w2" "$sd=0000"
run_case "refuses a remote file with a different SHA-256" 1 - 200 "$scratch/digest.json"
pypi_json "$scratch/extra.json" "$w1" "vmafx-0.1.0-cp314-cp314-macosx_11_0_arm64.whl=ab"
run_case "refuses a remote file this build did not produce" 1 - 200 "$scratch/extra.json"
pypi_json "$scratch/dup.json" "$w1" "$w1"
run_case "refuses duplicate remote records" 1 - 200 "$scratch/dup.json"
run_case "refuses any other HTTP status" 1 - 503

# Boundary: the local distribution set itself.
rm -f "$dist/$sd"
run_case "refuses a dist directory without the sdist" 1 - 404
make_dist "$dist"
rm -f "$dist"/*.whl
run_case "refuses a dist directory without wheels" 1 - 404
make_dist "$dist"
printf 'x' >"$dist/vmafx-0.1.1.tar.gz"
rm -f "$dist/$sd"
run_case "refuses an sdist of another version" 1 - 404
make_dist "$dist"
out_file="$scratch/github_output"
: >"$out_file"
(cd "$scratch" && PATH="$bin:$PATH" FAKE_PYPI_CODE=404 FAKE_PYPI_JSON="" \
  GITHUB_OUTPUT="$out_file" bash "$STATE" vmafx vmafx 0.1.0 "$dist" >/dev/null)
check "appends the decision to GITHUB_OUTPUT" grep -qx 'publish=true' "$out_file"
rc=0
bash "$STATE" vmafx vmafx >/dev/null 2>&1 || rc=$?
check "refuses a missing argument with usage exit 2" test "$rc" -eq 2

# shellcheck disable=SC2154  # pass and fail come from lib.sh
printf '%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
