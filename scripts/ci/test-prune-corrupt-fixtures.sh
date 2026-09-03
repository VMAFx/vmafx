#!/usr/bin/env bash
# scripts/ci/test-prune-corrupt-fixtures.sh — fixtures for prune-corrupt-fixtures.sh.
#
# The pruner deletes files, so its false-positive rate is the thing that
# matters: removing a healthy fixture costs a redundant download, but a rule
# loose enough to hit real payloads would delete the corpus. These cases pin
# both directions — every known corruption mode is removed, and a merely-short
# binary is kept, because "short" is not evidence of corruption.
#
#   bash scripts/ci/test-prune-corrupt-fixtures.sh
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
GATE="${repo_root}/scripts/ci/prune-corrupt-fixtures.sh"

if [ ! -x "${GATE}" ]; then
  echo "test-prune-corrupt-fixtures: pruner not executable: ${GATE}" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "${tmp}"' EXIT

ok=0
ng=0

# check <case> <removed|kept> <file>
check() {
  local name="$1" want="$2" f="$3"
  local got="kept"
  [ -e "${f}" ] || got="removed"
  if [ "${got}" = "${want}" ]; then
    ok=$((ok + 1))
    printf '  ok    %-34s %s\n' "${name}" "${got}"
  else
    ng=$((ng + 1))
    printf '  FAIL  %-34s want %s, got %s\n' "${name}" "${want}" "${got}"
  fi
}

root="${tmp}/resource"
mkdir -p "${root}/yuv"

: >"${root}/yuv/empty.yuv"
printf 'version https://git-lfs.github.com/spec/v1\noid sha256:d0\nsize 12\n' >"${root}/yuv/lfs.yuv"
printf '<!DOCTYPE html><html><body>404 Not Found</body></html>' >"${root}/yuv/html.yuv"
printf '<html><head><title>502</title></head></html>' >"${root}/yuv/html2.yuv"
printf '{"message":"Not Found","documentation_url":"https://docs.github.com"}' >"${root}/yuv/apierr.json"
head -c 1036800 /dev/urandom >"${root}/yuv/healthy.yuv"
# A real fixture can legitimately be tiny. Size alone must never condemn it.
head -c 300 /dev/urandom >"${root}/yuv/short_binary.yuv"
# A real JSON fixture that merely starts with a brace must survive.
printf '{"frames": [{"frameNum": 0, "metrics": {"vmaf": 97.4}}]}' >"${root}/scores.json"

echo "test-prune-corrupt-fixtures: running fixtures…"
"${GATE}" "${root}" >/dev/null

check "empty file" removed "${root}/yuv/empty.yuv"
check "git-lfs pointer" removed "${root}/yuv/lfs.yuv"
check "html error page (doctype)" removed "${root}/yuv/html.yuv"
check "html error page (bare)" removed "${root}/yuv/html2.yuv"
check "json api error" removed "${root}/yuv/apierr.json"
check "healthy payload" kept "${root}/yuv/healthy.yuv"
check "short but binary" kept "${root}/yuv/short_binary.yuv"
check "legitimate json fixture" kept "${root}/scores.json"

# A missing root is a no-op, not an error: the cache may simply not have been
# restored on a leg that does not run the Python harness.
if "${GATE}" "${tmp}/does-not-exist" >/dev/null 2>&1; then
  ok=$((ok + 1))
  printf '  ok    %-34s exit 0\n' "missing root is a no-op"
else
  ng=$((ng + 1))
  printf '  FAIL  %-34s expected exit 0\n' "missing root is a no-op"
fi

total=$((ok + ng))
echo "test-prune-corrupt-fixtures: ${ok}/${total} passed, ${ng} failed."
[ "${ng}" -eq 0 ]
