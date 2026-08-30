#!/usr/bin/env bash
# ffmpeg-patches-check.sh — local pre-push gate that mirrors the
# `ffmpeg-integration.yml` apply step. For each patch listed in
# `ffmpeg-patches/series.txt` (oldest to newest), applies the patch
# in series against a cached FFmpeg n8.1.1 checkout under
# `/tmp/ffmpeg-n81`. Patches in this stack build on each other (e.g.
# 0006 expects hunks 0003-0005 already in place), so the gate
# accumulates state and fails fast on the first patch that doesn't
# apply cleanly — the contributor sees the actual broken patch, not
# a cascade of derived failures.
#
# Network-down behaviour: if cloning / fetching fails (no network,
# rate-limit, mirror outage), exit 0 with a stderr warning. Local
# commits should not be blocked on connectivity.
#
# Exit codes:
#   0  — every patch in the series applies cleanly, OR the cache
#        could not be prepared and we degraded gracefully.
#   1  — at least one patch failed.

set -euo pipefail

# ---------------------------------------------------------------------------
# Resolve paths.
# ---------------------------------------------------------------------------
REPO_ROOT="$(git rev-parse --show-toplevel)"
PATCHES_DIR="${REPO_ROOT}/ffmpeg-patches"
SERIES_FILE="${PATCHES_DIR}/series.txt"
CACHE_DIR="${FFMPEG_PATCHES_CACHE:-/tmp/ffmpeg-n81}"
FFMPEG_REMOTE="${FFMPEG_PATCHES_REMOTE:-https://github.com/FFmpeg/FFmpeg}"
FFMPEG_BRANCH="${FFMPEG_PATCHES_BRANCH:-release/8.1}"

if [ ! -f "$SERIES_FILE" ]; then
  echo "ffmpeg-patches-check: ${SERIES_FILE} missing; nothing to check" >&2
  exit 0
fi

# ---------------------------------------------------------------------------
# Prepare cached FFmpeg checkout. Degrade to exit-0 on any network failure.
# ---------------------------------------------------------------------------
prep_cache() {
  if [ -d "${CACHE_DIR}/.git" ]; then
    if ! git -C "$CACHE_DIR" fetch --depth=1 origin "$FFMPEG_BRANCH" 2>/dev/null; then
      echo "ffmpeg-patches-check: WARN: fetch failed (offline?); using existing cache" >&2
      return 0
    fi
    git -C "$CACHE_DIR" reset --hard FETCH_HEAD >/dev/null
    return 0
  fi

  mkdir -p "$(dirname "$CACHE_DIR")"
  if ! git clone -q --depth=1 --branch "$FFMPEG_BRANCH" \
    "$FFMPEG_REMOTE" "$CACHE_DIR" 2>/dev/null; then
    echo "ffmpeg-patches-check: WARN: clone of ${FFMPEG_REMOTE}@${FFMPEG_BRANCH} failed (offline?); skipping" >&2
    return 1
  fi
  return 0
}

if ! prep_cache; then
  exit 0
fi

# Sanity-check the cache is usable.
if [ ! -d "${CACHE_DIR}/.git" ]; then
  echo "ffmpeg-patches-check: WARN: ${CACHE_DIR} is not a git checkout; skipping" >&2
  exit 0
fi

# ---------------------------------------------------------------------------
# Apply each patch in series order, accumulating state. Patches in this
# stack are NOT independent — each one builds on the changes the earlier
# ones introduce (e.g. 0006 adds entries to FFmpeg files that 0003-0005
# already wired up via earlier hunks). Re-basing before every iteration
# would be a false negative on every patch except 0001-0002.
#
# Reset to FETCH_HEAD once at the start; apply-and-keep each patch with
# `git apply` (not `--check` — we need the side effects so the next
# patch sees the right baseline). Fail fast on the first patch that
# does not apply cleanly so the contributor gets the actual broken
# patch, not a cascade of derived failures.
# ---------------------------------------------------------------------------
fail=0
patch_count=0
applied=()

# Clean both tracked and untracked state — earlier failed runs may leave
# new files (e.g. patch 0002 adds vf_vmaf_pre.c) that would cause the
# next invocation to fail with "already exists in working directory".
git -C "$CACHE_DIR" reset --hard -q HEAD
git -C "$CACHE_DIR" clean -fdq

while IFS= read -r line; do
  # Strip comments and blank lines.
  patch_name="${line%%#*}"
  patch_name="${patch_name## }"
  patch_name="${patch_name%% }"
  [ -z "$patch_name" ] && continue

  patch_path="${PATCHES_DIR}/${patch_name}"
  if [ ! -f "$patch_path" ]; then
    echo "FAIL: ${patch_name} listed in series.txt but missing under ffmpeg-patches/" >&2
    fail=$((fail + 1))
    continue
  fi

  patch_count=$((patch_count + 1))

  if git -C "$CACHE_DIR" apply "$patch_path" 2>/tmp/ffmpeg-patch-check.err; then
    echo "ok: ${patch_name}"
    applied+=("$patch_name")
  else
    echo "FAIL: ${patch_name} — git apply rejected the patch (with $(printf '%d' "${#applied[@]}") earlier patch(es) already applied)" >&2
    sed 's/^/    /' /tmp/ffmpeg-patch-check.err >&2 || true
    fail=$((fail + 1))
    break # Cascade after this point would be misleading — fail fast.
  fi
done <"$SERIES_FILE"

# Always reset the cache afterwards so the next invocation starts clean.
git -C "$CACHE_DIR" reset --hard -q HEAD
git -C "$CACHE_DIR" clean -fdq

rm -f /tmp/ffmpeg-patch-check.err

if [ "$fail" -gt 0 ]; then
  echo "ffmpeg-patches-check: stack-apply failed at patch ${patch_count} of $(grep -cvE '^[[:space:]]*(#|$)' "$SERIES_FILE") against ${FFMPEG_BRANCH}" >&2
  exit 1
fi

echo "ffmpeg-patches-check: ${patch_count} patches apply cleanly in series against ${FFMPEG_BRANCH}"
exit 0
