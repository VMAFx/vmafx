#!/usr/bin/env bash
# Drop fixtures that a previous download left in an unusable state, so the
# lazy fetcher re-downloads them instead of trusting them.
#
# compat/python-vmaf/config.py::download_reactively fetches a fixture only when
# the local file is ABSENT. A file that exists but is empty, or that holds an
# HTTP error page or a Git-LFS pointer instead of the payload, is therefore
# never repaired: it is "present", so the download is skipped, and the test
# fails with something unhelpful like `no frames decoded`. When such a file is
# captured into the CI fixture cache, every later run on that branch restores
# it and fails the same way until someone deletes the cache by hand.
#
# The success()-gated cache save stops NEW poisoning; this prunes what is
# already out there. Deleting a fixture is always safe — the worst case is one
# extra download.
#
# Usage: prune-corrupt-fixtures.sh [ROOT]   (default: python/test/resource)
set -euo pipefail
export LC_ALL=C

root="${1:-python/test/resource}"

if [ ! -d "${root}" ]; then
  printf 'prune-corrupt-fixtures: %s does not exist, nothing to do\n' "${root}"
  exit 0
fi

pruned=0
scanned=0

# A fixture is suspect when it is empty, or when its first bytes are text that
# no binary fixture would start with. Checked against the raw bytes so a
# truncated YUV that merely happens to be short is left alone — only a
# recognisable non-payload is removed.
while IFS= read -r -d '' f; do
  scanned=$((scanned + 1))
  reason=""

  if [ ! -s "${f}" ]; then
    reason="empty"
  else
    head_bytes=$(head -c 64 "${f}" 2>/dev/null | tr -d '\0' || true)
    case "${head_bytes}" in
      "version https://git-lfs"*)
        reason="Git-LFS pointer, not the payload"
        ;;
      "<!DOCTYPE"* | "<html"* | "<HTML"*)
        reason="HTML error page, not the payload"
        ;;
      "{"*'"message"'*)
        reason="JSON API error, not the payload"
        ;;
    esac
  fi

  if [ -n "${reason}" ]; then
    printf 'prune-corrupt-fixtures: removing %s (%s)\n' "${f}" "${reason}"
    rm -f -- "${f}"
    pruned=$((pruned + 1))
  fi
done < <(find "${root}" -type f -print0)

printf 'prune-corrupt-fixtures: scanned %d file(s), removed %d\n' "${scanned}" "${pruned}"
