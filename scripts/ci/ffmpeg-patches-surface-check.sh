#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/ffmpeg-patches-surface-check.sh — local + CI gate for
# agent hard rule 11 (docs/development/agent-hard-rules.md, ADR-0409): every PR that changes a
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
#       Reads PR body from $PR_BODY env var, or from stdin when fd 0 is
#       a pipe, a regular file or a socket. A terminal, a closed fd 0
#       and /dev/null carry no body; each is named and the gate runs on
#       the diff alone, where the opt-out is unclaimable and a moved
#       surface still fails. Classification lives in
#       scripts/ci/pr-body-input.sh — see it for why `[ ! -t 0 ]` is
#       the wrong test.
#       Diff is computed from $BASE_SHA..$HEAD_SHA env vars,
#       or falls back to $(git merge-base origin/master HEAD)..HEAD.
#
#   PR_BODY="$(gh pr view 999 --json body -q .body)" \
#       scripts/ci/ffmpeg-patches-surface-check.sh
#
# Exits 0 on PASS or no-public-surface-touched, 1 on a missing
# patch update, 2 on an environment / setup error.

set -euo pipefail

# A pipeline that ends in grep exits 1 when no line matched: that is data, not an
# error. Any other status (grep 2, git 128, a signal) is a failure and ends the
# check with the usage/error status instead of being read as "no symbols".
nomatch_ok() {
  if [ "$1" -eq 1 ]; then
    return 0
  fi
  echo "ffmpeg-patches-surface-check: a command failed with status $1." >&2
  exit 2
}

REPO_ROOT="${REPO_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
PATCHES_DIR="${REPO_ROOT}/ffmpeg-patches"

# ---------- 0. Invariant surface checks (Issue #2435, ADR-2166) ----------

check_surface_series() {
  local fail=0
  shopt -s nullglob
  local patches=("${PATCHES_DIR}"/*.patch)
  shopt -u nullglob
  if [ "${#patches[@]}" -eq 0 ]; then
    echo "ffmpeg-patches-surface-check: [FAIL] no patches found in ${PATCHES_DIR}" >&2
    return 1
  fi
  local p
  for p in "${patches[@]}"; do
    if grep -qE '^(---|\+\+\+)[[:space:]]+[ab]/libavfilter/vf_libvmaf\.c' "$p"; then
      echo "ffmpeg-patches-surface-check: [FAIL] patch touches retired vf_libvmaf.c: $p" >&2
      fail=1
    fi
    local m
    m="$(grep -nE '\b(libvmaf_(sycl|vulkan|metal|tune)|vmaf_pre)\b|-vmaf-profile\b|opt_vmaf_profile\b' "$p" 2>/dev/null || [ $? -eq 1 ])"
    if [ -n "$m" ]; then
      local filtered
      filtered="$(grep -vE 'libvmaf_(sycl|metal)\.h' <<<"$m" || [ $? -eq 1 ])"
      if [ -n "$filtered" ]; then
        echo "ffmpeg-patches-surface-check: [FAIL] patch carries retired vmaf filter/option in $p:" >&2
        printf '%s\n' "$filtered" >&2
        fail=1
      fi
    fi
    local m_cfg
    m_cfg="$(grep -nE '^\+[[:space:]]*--enable-libvmaf(-[a-z0-9_-]+)?\b' "$p" 2>/dev/null || [ $? -eq 1 ])"
    if [ -n "$m_cfg" ]; then
      echo "ffmpeg-patches-surface-check: [FAIL] patch adds retired configure flag in $p:" >&2
      printf '%s\n' "$m_cfg" >&2
      fail=1
    fi
  done
  if [ "$fail" -ne 0 ]; then
    return 1
  fi
  echo "ffmpeg-patches-surface-check: [PASS] series surface: zero vmaf-named filters/options in ${#patches[@]} patches."
  return 0
}

check_surface_fork_build_configs() {
  local fail=0
  local configs=(
    "Containerfile.vmafx"
    "Dockerfile"
    "Dockerfile.ffmpeg"
    "dev/Containerfile"
    "docker/Dockerfile.node"
    "scripts/test/sycl-dev-container.sh"
    ".github/workflows/ffmpeg-patch-stack.yml"
    "scripts/ci/tidy-integrations-build.sh"
    "ffmpeg-patches/test/build-and-run.sh"
  )
  local relpath fullpath
  for relpath in "${configs[@]}"; do
    fullpath="${REPO_ROOT}/${relpath}"
    [ -f "$fullpath" ] || continue
    local m_flag
    m_flag="$(grep -nE -- '--enable-libvmaf([^x]|$)' "$fullpath" 2>/dev/null || [ $? -eq 1 ])"
    if [ -n "$m_flag" ]; then
      echo "ffmpeg-patches-surface-check: [FAIL] fork build config has --enable-libvmaf (expected --enable-libvmafx): ${relpath}:" >&2
      printf '%s\n' "$m_flag" >&2
      fail=1
    fi
    local m_filt
    m_filt="$(grep -nE '\b(libvmaf_(sycl|vulkan|metal|tune)|vmaf_pre)\b|-vmaf-profile\b|--disable-filter=vmaf_pre' "$fullpath" 2>/dev/null || [ $? -eq 1 ])"
    if [ -n "$m_filt" ]; then
      local filtered
      filtered="$(grep -vE 'libvmaf_(sycl|metal)\.h' <<<"$m_filt" || [ $? -eq 1 ])"
      if [ -n "$filtered" ]; then
        echo "ffmpeg-patches-surface-check: [FAIL] fork build config references retired filter/option: ${relpath}:" >&2
        printf '%s\n' "$filtered" >&2
        fail=1
      fi
    fi
  done
  if [ "$fail" -ne 0 ]; then
    return 1
  fi
  echo "ffmpeg-patches-surface-check: [PASS] fork build configs: zero retired filters/options in ${#configs[@]} allow-listed configs."
  return 0
}

is_excluded_doc() {
  local p="$1"
  case "$p" in
    docs/adr/* | docs/adr.md | docs/research/* | docs/research.md) return 0 ;;
    docs/rebase-notes*) return 0 ;;
    docs/changelog-archive/* | docs/changelog-archive.md) return 0 ;;
    CHANGELOG.md | changelog.d/*) return 0 ;;
    docs/state.md | docs/backlog-audit-* | docs/backends/sycl/history.md) return 0 ;;
    *) return 1 ;;
  esac
}

scan_single_doc() {
  local relpath="$1"
  local fullpath="${REPO_ROOT}/${relpath}"
  [ -f "$fullpath" ] || return 0

  local content
  if [ "$relpath" = "docs/usage/ffmpeg.md" ]; then
    content="$(awk '
      /^## Migrating from the libvmaf filters/ { in_mig=1; next }
      in_mig && /^## / { in_mig=0 }
      !in_mig { print NR ":" $0 }
    ' "$fullpath")"
  else
    content="$(awk '{ print NR ":" $0 }' "$fullpath")"
  fi

  # Strip harmless non-retired references:
  # - libvmaf_*.h headers
  # - markdown link targets pointing to excluded adr/research files or anchors
  local cleaned
  cleaned="$(printf '%s\n' "$content" | sed -E \
    -e 's/libvmaf_[a-zA-Z0-9_]+\.h//g' \
    -e 's/\([^)]*(\/adr\/|\/research\/)[^)]*\)//g' \
    -e 's/#[a-zA-Z0-9_-]*libvmaf_[a-zA-Z0-9_-]*//g')"

  local doc_fail=0
  local pat_retired='\b(libvmaf_(sycl|vulkan|metal|cuda|hip|tune))\b|\bvmaf_pre\b|\bvmaf_pre_temporal\b|-vmaf-profile\b|--enable-libvmaf-[a-zA-Z0-9_-]+'
  if printf '%s\n' "$cleaned" | grep -E "$pat_retired" >/dev/null; then
    echo "ffmpeg-patches-surface-check: [FAIL] retired filter/option in doc ${relpath}:" >&2
    printf '%s\n' "$cleaned" | grep -E "$pat_retired" >&2
    doc_fail=1
  fi

  # shellcheck disable=SC2016 # literal backticks in a grep pattern, nothing to expand
  local pat_plain='--enable-libvmaf\b|(\bfilter\b[^\n`\(\)]*?\blibvmaf\b|\blibvmaf\b[^\n`\(\)]*?\bfilter\b)|-lavfi\s+[^\n]*\blibvmaf\b|-filter_complex\s+[^\n]*\blibvmaf\b'
  if printf '%s\n' "$cleaned" | grep -E -e "$pat_plain" >/dev/null; then
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      if ! echo "$line" | grep -qiE "upstream|stock|unpatched|backwards|backward|compat"; then
        echo "ffmpeg-patches-surface-check: [FAIL] plain libvmaf filter/configure flag without stock upstream qualification in doc ${relpath}:" >&2
        printf '%s\n' "$line" >&2
        doc_fail=1
      fi
    done < <(printf '%s\n' "$cleaned" | grep -E -e "$pat_plain")
  fi

  return "$doc_fail"
}

check_surface_docs_examples() {
  local fail=0
  local all_docs=()
  if git -C "${REPO_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    mapfile -t all_docs < <(git -C "${REPO_ROOT}" ls-files "docs/*.md" "docs/**/*.md" "README.md" "ffmpeg-patches/README.md")
  else
    mapfile -t all_docs < <(cd "${REPO_ROOT}" && find docs README.md ffmpeg-patches/README.md -name "*.md" 2>/dev/null | sed "s|^\./||")
  fi

  local relpath
  local doc_count=0
  for relpath in "${all_docs[@]}"; do
    is_excluded_doc "$relpath" && continue
    [ -f "${REPO_ROOT}/${relpath}" ] || continue
    doc_count=$((doc_count + 1))
    if ! scan_single_doc "$relpath"; then
      fail=1
    fi
  done

  if [ "$fail" -ne 0 ]; then
    return 1
  fi
  echo "ffmpeg-patches-surface-check: [PASS] docs examples: zero retired filters/options outside migration guide across ${doc_count} scanned doc files."
  return 0
}

is_allowlisted_surface_file() {
  local p="$1"
  case "$p" in
    # Exact allow-listed files with justification (ADR-2166):
    ffmpeg-patches/test/fault_inject_libvmaf.c)
      # Reason: LD_PRELOAD fault injection shim intercepting dynamic symbols
      return 0
      ;;
    scripts/ci/upstream-ffmpeg-compat.sh)
      # Reason: unpatched stock upstream FFmpeg compatibility test
      return 0
      ;;
    scripts/ci/upstream-gstreamer-compat.sh)
      # Reason: unpatched stock upstream GStreamer compatibility test
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

check_surface_patch_tests() {
  local fail=0
  shopt -s nullglob
  local test_scripts=("${PATCHES_DIR}/test"/*.sh)
  shopt -u nullglob
  if [ "${#test_scripts[@]}" -eq 0 ]; then
    echo "ffmpeg-patches-surface-check: [FAIL] no test scripts found in ${PATCHES_DIR}/test" >&2
    return 1
  fi
  local s
  local pat_retired='\b(libvmaf_(sycl|vulkan|metal|cuda|hip|tune))\b|\bvmaf_pre\b|\bvmaf_pre_temporal\b|-vmaf-profile\b|--enable-libvmaf-[a-zA-Z0-9_-]+'
  local pat_plain='libvmaf=[^[:space:]]+|\b(filter_complex|lavfi)\b.*\blibvmaf\b'
  for s in "${test_scripts[@]}"; do
    local relpath
    relpath="ffmpeg-patches/test/$(basename "$s")"
    is_allowlisted_surface_file "$relpath" && continue
    local content
    content="$(grep -vE 'fault_inject_libvmaf\.c' "$s" 2>/dev/null || [ $? -eq 1 ])"
    local m
    m="$(grep -nE "$pat_retired" <<<"$content" 2>/dev/null || [ $? -eq 1 ])"
    if [ -n "$m" ]; then
      echo "ffmpeg-patches-surface-check: [FAIL] patch test carries retired filter/option in ${relpath}:" >&2
      printf '%s\n' "$m" >&2
      fail=1
    fi
    local m_plain
    m_plain="$(grep -nE "$pat_plain" <<<"$content" 2>/dev/null || [ $? -eq 1 ])"
    if [ -n "$m_plain" ]; then
      echo "ffmpeg-patches-surface-check: [FAIL] patch test carries plain libvmaf filter in ${relpath}:" >&2
      printf '%s\n' "$m_plain" >&2
      fail=1
    fi
  done
  if [ "$fail" -ne 0 ]; then
    return 1
  fi
  echo "ffmpeg-patches-surface-check: [PASS] patch tests: zero retired filters/options in ${#test_scripts[@]} test scripts."
  return 0
}

# Run #2435 invariant surface checks
check_surface_series || exit 1
check_surface_fork_build_configs || exit 1
check_surface_docs_examples || exit 1
check_surface_patch_tests || exit 1

# ---------- 1. Locate PR body ----------

# Shared with deliverables-check.sh / validate-pr-body.sh / state-md-touch-
# check.sh so every gate that reads a PR body decides what fd 0 is the same
# way. Not `[ ! -t 0 ]`: that asks whether fd 0 is a terminal, which is a
# different question from whether anybody piped a body, and `PR_BODY="$(cat)"`
# on a closed fd 0 does not fail — it deadlocks. See
# scripts/ci/pr-body-input.sh.
_ffmpeg_surface_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/ci/pr-body-input.sh
. "${_ffmpeg_surface_dir}/pr-body-input.sh"
# shellcheck source=scripts/ci/pr-diff-base.sh
. "${_ffmpeg_surface_dir}/pr-diff-base.sh"

if [ -n "${PR_BODY:-}" ]; then
  body_src="env"
else
  # Deliberately `-n`, not the `+x` "set counts as an answer" rule
  # deliverables-check.sh uses. That gate reports a set-but-blank PR_BODY
  # differently from an empty pipe; here both branches end at the same empty
  # string, so an explicitly blank PR_BODY may still fall through to a pipe
  # that does carry a body.
  pr_body_classify_stdin
  if [ "${PR_BODY_STDIN_KIND}" = "stream" ]; then
    PR_BODY="$(pr_body_read_stdin)"
    pr_body_close_stdin
    body_src="stdin"
  else
    # Unlike deliverables-check.sh, an absent body is legal here: the opt-out
    # becomes unclaimable, so the gate falls through to the diff check and
    # fails closed if a consumed surface moved. Name what fd 0 was, so a CI
    # step that meant to pipe a body can see that it did not.
    PR_BODY=""
    body_src="empty (${PR_BODY_STDIN_KIND} stdin)"
  fi
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
  # From the merge base, never from BASE_SHA itself: see pr-diff-base.sh.
  diff_base="$(pr_diff_base "${BASE_SHA}" "${HEAD_SHA}" ffmpeg-patches-surface-check)" || exit 2
  diff_head="${HEAD_SHA}"
  diff_src="env (merge base of BASE_SHA and HEAD_SHA..HEAD_SHA)"
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
    >"$tmp_consumed" || nomatch_ok $?

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
header_diff="$(git diff "${diff_base}..${diff_head}" -- 'core/include/libvmaf/*.h' || nomatch_ok $?)"

if [ -n "$header_diff" ]; then
  printf '%s\n' "$header_diff" |
    grep -E '^[+-][^+-]' |
    sed -E 's|//.*$||; s|/\*.*\*/||' |
    grep -hoE '\b(vmaf_[a-zA-Z0-9_]+|Vmaf[A-Z][a-zA-Z0-9]+)\b' |
    sort -u >"$tmp_diff_syms" || nomatch_ok $?
fi

# meson_options.txt changes — capture any `option('<name>'` declaration
# touched in `+` or `-` lines.
meson_diff="$(git diff "${diff_base}..${diff_head}" -- 'core/meson_options.txt' || nomatch_ok $?)"

if [ -n "$meson_diff" ]; then
  printf '%s\n' "$meson_diff" |
    grep -E "^[+-][^+-].*option\('" |
    grep -hoE "option\('[a-zA-Z0-9_]+'" |
    sed -E "s/option\('//; s/'\$//" |
    sort -u >"$tmp_diff_flags" || nomatch_ok $?
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
  sym_hits="$(grep -F -x -f "$tmp_diff_syms" "$tmp_consumed" || nomatch_ok $?)"
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
echo "::error title=Agent hard rule 11::Public libvmaf surface consumed by ffmpeg-patches/ was changed without a patch update."
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
echo "Per agent hard rule 11 (docs/development/agent-hard-rules.md, ADR-0409), every PR that"
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
