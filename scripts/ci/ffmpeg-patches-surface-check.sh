#!/usr/bin/env bash
# scripts/ci/ffmpeg-patches-surface-check.sh — local + CI gate for
# CLAUDE.md §12 r14 (ADR-0186 + ADR-0356): every PR that changes a
# libvmaf public-surface symbol consumed by ffmpeg-patches/*.patch
# must update at least one patch file in the same PR.
#
# Mirrors the deliverables-check.sh / ffmpeg-patches-check.sh
# convention so a contributor can run the same gate locally before
# `gh pr create`. Reads the PR body for an opt-out line, computes
# the diff between BASE_SHA..HEAD_SHA (or merge-base origin/master),
# and intersects the diff against a "consumed set" extracted from
# the patches under ffmpeg-patches/.
#
# Detection algorithm (bash + grep, no AST):
#   1. Parse ffmpeg-patches/*.patch once, extract the union of:
#        - vmaf_<ident>          (libvmaf public C symbols)
#        - Vmaf<TitleCase>       (libvmaf public types: VmafModelConfig, VmafLogLevel, ...)
#        - --enable-libvmaf-*    (FFmpeg configure flags)
#        - libvmaf_<ident>       (pkg-config feature names: libvmaf_sycl, libvmaf_vulkan, ...)
#      Cache as a regex-friendly pipe-joined union in $tmp_consumed.
#
#   2. For the diff: collect every added/removed/renamed identifier
#      from non-comment lines under core/include/libvmaf/*.h, and
#      from lines under core/meson_options.txt that touch an
#      `option('<name>'` declaration.
#
#   3. If the consumed-set intersects the diff-set AND no
#      ffmpeg-patches/*.patch is in the diff → fail.
#
#   4. Opt-out: PR body line `no ffmpeg-patches update needed: <reason>`.
#
# Usage:
#   scripts/ci/ffmpeg-patches-surface-check.sh
#       Reads PR body from $PR_BODY env var or stdin.
#       Diff is computed from $BASE_SHA..$HEAD_SHA env vars,
#       or falls back to $(git merge-base origin/master HEAD)..HEAD.
#
#   PR_BODY="$(gh pr view 999 --json body -q .body)" \
#       scripts/ci/ffmpeg-patches-surface-check.sh
#
# Exits 0 on PASS or no-public-surface-touched, 1 on a missing
# patch update, 2 on an environment / setup error.

set -euo pipefail

# ---------- 1. Locate PR body ----------

if [ -n "${PR_BODY:-}" ]; then
  body_src="env"
elif [ ! -t 0 ]; then
  PR_BODY="$(cat)"
  body_src="stdin"
else
  # Empty body is fine — opt-out is impossible, but if no surface is
  # touched the gate still passes. Treat as "no body supplied".
  PR_BODY=""
  body_src="empty"
fi

tmp_body="$(mktemp)"
tmp_diff="$(mktemp)"
tmp_consumed="$(mktemp)"
tmp_diff_syms="$(mktemp)"
tmp_diff_flags="$(mktemp)"
trap 'rm -f "$tmp_body" "$tmp_diff" "$tmp_consumed" "$tmp_diff_syms" "$tmp_diff_flags"' EXIT

# Strip markdown emphasis for opt-out matching, mirroring
# deliverables-check.sh.
# shellcheck disable=SC1003  # the trailing \\ is a tr-recognised escape for a literal backslash
printf '%s' "${PR_BODY}" | tr -d '`*_\\' >"$tmp_body"

# ---------- 2. Locate diff base ----------

if [ -n "${BASE_SHA:-}" ] && [ -n "${HEAD_SHA:-}" ]; then
  diff_base="${BASE_SHA}"
  diff_head="${HEAD_SHA}"
  diff_src="env (BASE_SHA..HEAD_SHA)"
else
  if ! git rev-parse --verify origin/master >/dev/null 2>&1; then
    echo "ffmpeg-patches-surface-check: origin/master not found; run 'git fetch origin master' first." >&2
    exit 2
  fi
  diff_base="$(git merge-base origin/master HEAD)"
  diff_head="HEAD"
  diff_src="auto (merge-base origin/master..HEAD)"
fi

git diff --name-only "${diff_base}..${diff_head}" >"$tmp_diff"

echo "ffmpeg-patches-surface-check: PR body from ${body_src}, diff from ${diff_src}"

# ---------- 3. Early opt-out ----------

if grep -qiE "no ffmpeg-patches update needed[: ]" "$tmp_body"; then
  echo "ffmpeg-patches-surface-check: opt-out claimed in PR body ('no ffmpeg-patches update needed: ...'). PASS."
  exit 0
fi

# ---------- 4. Build the "consumed set" from patches ----------

REPO_ROOT="$(git rev-parse --show-toplevel)"
PATCHES_DIR="${REPO_ROOT}/ffmpeg-patches"

if [ ! -d "$PATCHES_DIR" ]; then
  echo "ffmpeg-patches-surface-check: ${PATCHES_DIR} missing; nothing to check." >&2
  exit 0
fi

# Concatenate added + context lines from every patch, then extract:
#   - vmaf_[a-zA-Z0-9_]+          public C symbol names
#   - Vmaf[A-Z][a-zA-Z0-9]+       public C type names (CamelCase)
#   - libvmaf_[a-zA-Z0-9_]+       pkg-config feature names
#   - --enable-libvmaf[a-zA-Z0-9_-]*   FFmpeg configure flags
#
# Then sort -u and pipe-join into a regex alternation.
#
# Filter false positives: drop tokens that are too short (<= 5 chars)
# or that match a small ignore list of generic tokens like "vmaf_v0"
# (pkg-config version literal, not a libvmaf symbol).
{
  cat "${PATCHES_DIR}"/*.patch 2>/dev/null
} | grep -hoE '\b(vmaf_[a-zA-Z0-9_]+|Vmaf[A-Z][a-zA-Z0-9]+|libvmaf_[a-zA-Z0-9_]+|--enable-libvmaf[a-zA-Z0-9_-]*)\b' |
  sort -u |
  grep -vE '^vmaf_v[0-9]+$' |
  grep -vE '^vmaf_(pre|tune)$' \
    >"$tmp_consumed" || true

consumed_count=$(wc -l <"$tmp_consumed" | tr -d ' ')
if [ "${consumed_count}" -eq 0 ]; then
  echo "ffmpeg-patches-surface-check: WARN: extracted zero symbols from ${PATCHES_DIR}/*.patch; pattern likely broken." >&2
  exit 2
fi

# ---------- 5. Build the "diff set" from headers + meson_options ----------

# Public-header changes — strip C-style line comments and block comments
# (best-effort: a single-line `/* ... */` is removed; multi-line block
# comments are not stripped — we accept the rare false positive of a
# symbol mentioned only in a multi-line comment because the cost is
# low: an extra patch update or a one-line opt-out).
#
# We extract identifiers from the union of `+` and `-` lines (added or
# removed in the diff). Pure `+ // comment` or `+ /* comment */` lines
# are filtered.
header_diff="$(git diff "${diff_base}..${diff_head}" -- 'core/include/libvmaf/*.h' || true)"

if [ -n "$header_diff" ]; then
  printf '%s\n' "$header_diff" |
    grep -E '^[+-][^+-]' |
    sed -E 's|//.*$||; s|/\*.*\*/||' |
    grep -hoE '\b(vmaf_[a-zA-Z0-9_]+|Vmaf[A-Z][a-zA-Z0-9]+)\b' |
    sort -u >"$tmp_diff_syms" || true
fi

# meson_options.txt changes — capture any `option('<name>'` declaration
# touched in `+` or `-` lines.
meson_diff="$(git diff "${diff_base}..${diff_head}" -- 'core/meson_options.txt' || true)"

if [ -n "$meson_diff" ]; then
  printf '%s\n' "$meson_diff" |
    grep -E "^[+-][^+-].*option\('" |
    grep -hoE "option\('[a-zA-Z0-9_]+'" |
    sed -E "s/option\('//; s/'\$//" |
    sort -u >"$tmp_diff_flags" || true
fi

# ---------- 6. Check intersection ----------

# Did the diff touch any public header at all (even if only comments)?
header_touched=0
if [ -n "$header_diff" ]; then
  header_touched=1
fi

# Did the diff touch meson_options.txt?
meson_touched=0
if [ -n "$meson_diff" ]; then
  meson_touched=1
fi

# Did the diff touch any ffmpeg-patches/*.patch file?
patch_touched=0
if grep -qE '^ffmpeg-patches/.*\.patch$' "$tmp_diff"; then
  patch_touched=1
fi

# Compute symbol-level intersections.
sym_hit_count=0
flag_hit_count=0
sym_hits=""
flag_hits=""

if [ -s "$tmp_diff_syms" ]; then
  # For every diff symbol, check if it appears verbatim in the consumed
  # set. We use grep -F -x for an exact line-match: each line of
  # tmp_consumed and tmp_diff_syms is one identifier.
  sym_hits="$(grep -F -x -f "$tmp_diff_syms" "$tmp_consumed" || true)"
  if [ -n "$sym_hits" ]; then
    sym_hit_count=$(printf '%s\n' "$sym_hits" | wc -l | tr -d ' ')
  fi
fi

# meson_options names are surfaced in patches as `--enable-libvmaf-<name>`
# or as `libvmaf_<name>` pkg-config names, so map each touched option to
# both forms before checking.
if [ -s "$tmp_diff_flags" ]; then
  while IFS= read -r opt; do
    [ -z "$opt" ] && continue
    # Strip the leading `enable_` or `built_in_` etc — patches reference
    # the bare suffix as `libvmaf_<suffix>`. We try both the full name and
    # the suffix-after-`enable_`.
    candidates=("$opt")
    if [[ "$opt" == enable_* ]]; then
      candidates+=("libvmaf_${opt#enable_}" "--enable-libvmaf-${opt#enable_}")
    fi
    for c in "${candidates[@]}"; do
      if grep -F -x -q "$c" "$tmp_consumed"; then
        flag_hits="${flag_hits}${opt} (matched ${c})\n"
        flag_hit_count=$((flag_hit_count + 1))
        break
      fi
    done
  done <"$tmp_diff_flags"
fi

# ---------- 7. Decide ----------

# If neither headers nor meson_options were touched, nothing to gate.
if [ "$header_touched" -eq 0 ] && [ "$meson_touched" -eq 0 ]; then
  echo "ffmpeg-patches-surface-check: no public-header or meson_options changes in this PR. PASS."
  exit 0
fi

# If a public surface was touched but no symbol overlap with the
# consumed set, treat as comment-only / unrelated and pass with a note.
if [ "$sym_hit_count" -eq 0 ] && [ "$flag_hit_count" -eq 0 ]; then
  echo "ffmpeg-patches-surface-check: public surface touched but no symbol consumed by ffmpeg-patches/ changed. PASS (likely doc / comment-only)."
  exit 0
fi

# Symbol overlap — require a patch update.
if [ "$patch_touched" -eq 1 ]; then
  echo "ffmpeg-patches-surface-check: ${sym_hit_count} consumed symbol(s) and ${flag_hit_count} consumed flag(s) touched, and at least one ffmpeg-patches/*.patch is in the diff. PASS."
  if [ -n "$sym_hits" ]; then
    echo "  consumed symbols touched:"
    printf '%s\n' "$sym_hits" | sed 's/^/    - /'
  fi
  if [ -n "$flag_hits" ]; then
    echo "  consumed flags touched:"
    printf '%b' "$flag_hits" | sed 's/^/    - /'
  fi
  exit 0
fi

# Hard fail.
echo "::error title=CLAUDE.md §12 r14::Public libvmaf surface consumed by ffmpeg-patches/ was changed without a patch update."
echo ""
if [ -n "$sym_hits" ]; then
  echo "  consumed symbols touched:"
  printf '%s\n' "$sym_hits" | sed 's/^/    - /'
fi
if [ -n "$flag_hits" ]; then
  echo "  consumed flags touched:"
  printf '%b' "$flag_hits" | sed 's/^/    - /'
fi
echo ""
echo "Per CLAUDE.md §12 r14 (and ADR-0186 / ADR-0356), every PR that"
echo "changes a public libvmaf surface consumed by ffmpeg-patches/"
echo "must update at least one patch file in the SAME PR — otherwise"
echo "the next /sync-upstream rebase inherits a silently-broken"
echo "FFmpeg integration build."
echo ""
echo "Either:"
echo "  1. Update the relevant ffmpeg-patches/*.patch file in this PR, OR"
echo "  2. If the change is genuinely patch-irrelevant (e.g. a typo fix"
echo "     in a doxygen comment that happened to include the symbol),"
echo "     add a line to the PR description:"
echo "       no ffmpeg-patches update needed: <reason>"
echo ""
echo "See docs/development/automated-rule-enforcement.md for details."
exit 1
