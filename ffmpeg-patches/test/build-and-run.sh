#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# build-and-run.sh — smoke-test the ffmpeg-patches/ series against a pinned
# upstream FFmpeg release tag from build-config.env.
#
# What it does:
#   1. Fetches FFmpeg into a new, disposable $FFMPEG_SRC.
#   2. Applies every patch listed in ffmpeg-patches/series.txt (comment-
#      stripped) in order.
#   3. Configures with the minimum libraries needed for libvmaf + vmaf_pre,
#      builds ffmpeg, runs FFmpeg's generated FATE subset, and verifies:
#        - `ffmpeg -h filter=libvmaf` lists `tiny_model`
#        - `ffmpeg -h filter=vmaf_pre` exits 0
#   4. With VMAF_SCORE_CHECK=1, scores the 48 frames of the testdata 576x324
#      pair with the libvmaf filter and with $VMAF_PREFIX/bin/vmaf, and
#      requires the same text for every frame and pooled metric.
#   5. Tears down the build tree unless $KEEP_BUILD is set.
#
# Requires libvmaf already installed (`pip`-level: "pkg-config --cflags libvmaf"
# must resolve). Set VMAF_PREFIX to point at a non-standard install prefix.
#
# Settings (environment):
#   FFMPEG_TOOLCHAIN   empty (the host's cc) or msvc: cl.exe through FFmpeg's
#                      --toolchain=msvc, run from an MSYS2 shell that has the
#                      MSVC environment (docs/getting-started/building-on-windows.md)
#   FFMPEG_JOBS        make parallelism (default: nproc)
#   SMOKE_FATE         1 (default) runs the FATE subset; 0 skips it, said so
#   VMAF_SCORE_CHECK   0 (default) or 1, see step 4; needs VMAF_PREFIX
#
# Applies the complete ordered series with `git am --3way`.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES_DIR="$(cd "$HERE/.." && pwd)"

# Shared reviewed release tag; an override must also be a stable release.
# shellcheck source=../../build-config.env
# shellcheck disable=SC1091
source "${PATCHES_DIR}/../build-config.env"
: "${FFMPEG_SHA:=${FFMPEG_TAG}}"
if [[ ! "$FFMPEG_SHA" =~ ^n[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
  echo "FFMPEG_SHA must be a stable released tag: $FFMPEG_SHA" >&2
  exit 1
fi
: "${KEEP_BUILD:=}"
: "${VMAF_PREFIX:=}"
: "${FFMPEG_TOOLCHAIN:=}"
: "${FFMPEG_JOBS:=}"
: "${SMOKE_FATE:=1}"
: "${VMAF_SCORE_CHECK:=0}"

# Settings are checked before anything is fetched or built.
toolchain_args=()
case "$FFMPEG_TOOLCHAIN" in
  '') ;;
  msvc)
    # Every object of one MSVC link must use the same C runtime. A Meson
    # release build of VMAFx compiles with /MD (b_vscrt=from_buildtype), so
    # FFmpeg does too; the linker reports a mixed runtime as warning LNK4098,
    # which the warning gate below fails on.
    toolchain_args=(--toolchain=msvc --extra-cflags=-MD --extra-cxxflags=-MD)
    ;;
  *)
    echo "FFMPEG_TOOLCHAIN must be empty or msvc: $FFMPEG_TOOLCHAIN" >&2
    exit 1
    ;;
esac
if [[ ! "$SMOKE_FATE" =~ ^[01]$ ]]; then
  echo "SMOKE_FATE must be 0 or 1: $SMOKE_FATE" >&2
  exit 1
fi
if [[ ! "$VMAF_SCORE_CHECK" =~ ^[01]$ ]]; then
  echo "VMAF_SCORE_CHECK must be 0 or 1: $VMAF_SCORE_CHECK" >&2
  exit 1
fi
if [[ "$VMAF_SCORE_CHECK" == 1 && -z "$VMAF_PREFIX" ]]; then
  echo "VMAF_SCORE_CHECK=1 needs VMAF_PREFIX (the install whose bin/vmaf scores)" >&2
  exit 1
fi
if [[ -z "$FFMPEG_JOBS" ]]; then
  FFMPEG_JOBS="$(nproc)"
fi
if [[ ! "$FFMPEG_JOBS" =~ ^[1-9][0-9]{0,2}$ ]]; then
  echo "FFMPEG_JOBS must be an integer from 1 to 999: $FFMPEG_JOBS" >&2
  exit 1
fi

if [[ -n "$VMAF_PREFIX" ]]; then
  export PKG_CONFIG_PATH="$VMAF_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  export LD_LIBRARY_PATH="$VMAF_PREFIX/lib:${LD_LIBRARY_PATH:-}"
fi

# Refuse existing workspaces before touching any build input.
if [[ -n "${FFMPEG_SRC:-}" && (-e "$FFMPEG_SRC" || -L "$FFMPEG_SRC") ]]; then
  echo "FFMPEG_SRC must be a new path; existing checkout preserved: $FFMPEG_SRC" >&2
  exit 1
fi

if ! command -v git >/dev/null; then
  echo "git not found on PATH" >&2
  exit 77
fi
if ! pkg-config --exists libvmaf; then
  echo "libvmaf not found via pkg-config — install libvmaf first or set VMAF_PREFIX" >&2
  exit 77
fi

# A hook caller's index/object-store must never affect this disposable clone.
for git_variable in ${!GIT_@}; do
  unset "$git_variable"
done
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0
ffmpeg_git() {
  command git -c core.hooksPath=/dev/null -c commit.gpgsign=false \
    -c user.name='VMAFx smoke test' -c user.email=smoke@localhost "$@"
}

checkout_parent=""
if [[ -z "${FFMPEG_SRC:-}" ]]; then
  checkout_parent="$(mktemp -d -t vmafx-ffmpeg-smoke.XXXXXXXX)"
  FFMPEG_SRC="${checkout_parent}/ffmpeg"
fi
echo "Cloning FFmpeg into $FFMPEG_SRC …"
"${PATCHES_DIR}/../scripts/ci/checkout-annotated-tag.sh" \
  "$FFMPEG_REMOTE" "$FFMPEG_SHA" "$FFMPEG_SRC"

echo "Applying patches from $PATCHES_DIR …"
# The MSVC warning gate below reads the files the series adds or changes.
series_base="$(ffmpeg_git -C "$FFMPEG_SRC" rev-parse HEAD)"
while IFS= read -r line; do
  line="${line%%#*}"
  line="${line// /}"
  # A CRLF checkout (Windows, `text=auto`) leaves a CR on every line.
  line="${line//$'\r'/}"
  [[ -z "$line" ]] && continue
  echo "  → $line"
  ffmpeg_git -C "$FFMPEG_SRC" am --3way "$PATCHES_DIR/$line"
done <"$PATCHES_DIR/series.txt"

echo "Configuring FFmpeg …"
cd "$FFMPEG_SRC"
# The empty-array form keeps `set -u` quiet on bash 3.2 (macOS /bin/bash).
./configure \
  --fatal-warnings \
  --disable-doc \
  --disable-debug \
  --disable-programs \
  --enable-ffmpeg \
  --enable-libvmaf \
  --enable-filter=vmaf_pre \
  --enable-gpl \
  ${toolchain_args[@]+"${toolchain_args[@]}"}

echo "Building FFmpeg …"
build_log="$FFMPEG_SRC/vmafx-build.log"
make -j"$FFMPEG_JOBS" build 2>&1 | tee "$build_log"
if [[ "$SMOKE_FATE" == 1 ]]; then
  echo "Running generated FATE subset …"
  fate_list="$(make -s fate-list)"
  fate_targets=()
  while IFS= read -r target; do
    fate_targets+=("$target")
  done < <(printf '%s\n' "$fate_list" | awk '/^fate-/')
  if ((${#fate_targets[@]} == 0)); then
    echo "FFmpeg generated no local FATE targets" >&2
    exit 1
  fi
  make -j"$FFMPEG_JOBS" "${fate_targets[@]}" 2>&1 | tee -a "$build_log"
else
  echo "SKIP: FATE subset not run (SMOKE_FATE=0)"
fi
# cl reports some 400 warnings in FFmpeg's own sources (C4334, C4113, C5287,
# ...) and D9024 when FFmpeg links its host tools, none of which the series
# writes; some sit in files the series also edits (libavcodec/vlc.c). Under
# msvc the gate therefore refuses every compiler warning on a line the series
# adds or changes (the + side of `git diff -U0` from the release to the
# patched head, matched by path and line; cl prints `file(line): warning
# Cnnnn` or `file(line,col): ...`, with `\` or `/`, sometimes prefixed `.\`),
# and every linker warning. The other toolchains refuse every warning.
msvc_findings() {
  ffmpeg_git -C "$FFMPEG_SRC" diff -U0 --no-color "$series_base" HEAD |
    awk -v log_file="$1" '
      /^\+\+\+ b\// { file = substr($0, 7); files[file] = 1; next }
      /^\+\+\+ / { file = ""; next }
      /^@@ / && file != "" {
        n = split($3, hunk, ",")
        start = substr(hunk[1], 2) + 0
        count = (n > 1) ? hunk[2] + 0 : 1
        for (i = 0; i < count; i++) changed[file ":" (start + i)] = 1
        next
      }
      END {
        while ((status = (getline line < log_file)) > 0) {
          if (line ~ /warning LNK[0-9]+/) { print line; continue }
          if (!match(line, /\([0-9]+(,[0-9]+)?\): warning C[0-9]+/)) continue
          path = substr(line, 1, RSTART - 1)
          num = substr(line, RSTART + 1)
          sub(/[,)].*/, "", num)
          gsub(/\\/, "/", path)
          sub(/^\.\//, "", path)
          for (f in files) {
            if (path == f || substr(path, length(path) - length(f)) == "/" f) {
              if ((f ":" num) in changed) print line
              break
            }
          }
        }
        if (status < 0) {
          print "cannot read " log_file > "/dev/stderr"
          exit 2
        }
      }'
}
if [[ "$FFMPEG_TOOLCHAIN" == msvc ]]; then
  warnings="$(msvc_findings "$build_log")"
else
  warnings="$(grep -Ei '(^|[[:space:]])warning([[:space:]#:])' "$build_log" || [[ $? -eq 1 ]])"
fi
if [[ -n "$warnings" ]]; then
  printf '%s\n' "$warnings" >&2
  echo "FFmpeg emitted compiler warnings" >&2
  exit 1
fi
rm "$build_log"

echo "Verifying new options …"
./ffmpeg -hide_banner -h filter=libvmaf 2>&1 | grep -q -- 'tiny_model' ||
  {
    echo "libvmaf filter does not advertise tiny_model"
    exit 1
  }
./ffmpeg -hide_banner -h filter=vmaf_pre >/dev/null 2>&1 ||
  {
    echo "vmaf_pre filter not registered"
    exit 1
  }

# score_check: the libvmaf filter of this build and the vmaf CLI of
# $VMAF_PREFIX score the same frames; every frame and pooled metric must be the
# same text. The helpers are the upstream-consumer checks' (HISS-19).
score_check() {
  # shellcheck source=scripts/ci/upstream-consumer-lib.sh
  # shellcheck disable=SC1091  # followed with -x; the hook lints the file alone
  . "$PATCHES_DIR/../scripts/ci/upstream-consumer-lib.sh"
  uc_defaults
  PREFIX="$VMAF_PREFIX"
  REF_YUV="$UC_REPO/testdata/ref_576x324_48f.yuv"
  DIST_YUV="$UC_REPO/testdata/dis_576x324_48f.yuv"
  WORK="$FFMPEG_SRC/vmafx-score"
  # All 48 frames: the first eight of this pair are close to identical.
  # shellcheck disable=SC2034  # read by uc_validate and uc_ffmpeg_graph
  FRAMES=48
  uc_validate
  # The log path is relative to the working directory: on MSYS2 an absolute
  # POSIX path inside a filter graph is not converted for a native ffmpeg.exe.
  (
    cd "$WORK"
    timeout 300 "$FFMPEG_SRC/ffmpeg" -hide_banner -nostdin -loglevel info \
      -f rawvideo -pix_fmt yuv420p -video_size "$SIZE" -i "$DIST_YUV" \
      -f rawvideo -pix_fmt yuv420p -video_size "$SIZE" -i "$REF_YUV" \
      -lavfi "$(uc_ffmpeg_graph ffmpeg.json)" -an -f null - >ffmpeg.log 2>&1
  ) || uc_die "ffmpeg scoring failed (see $WORK/ffmpeg.log)"
  [[ -s "$WORK/ffmpeg.json" ]] || uc_die "ffmpeg wrote no $WORK/ffmpeg.json"
  uc_cli_scores "$PREFIX" "$WORK/cli.json"
  uc_compare "$WORK/ffmpeg.json" "$WORK/cli.json" ffmpeg vmaf-cli
}

if [[ "$VMAF_SCORE_CHECK" == 1 ]]; then
  echo "Comparing libvmaf filter scores with the vmaf CLI …"
  score_check
fi

echo "PASS: ffmpeg-patches smoke ok"

if [[ -z "$KEEP_BUILD" ]]; then
  echo "Cleaning $FFMPEG_SRC (set KEEP_BUILD=1 to keep)."
  rm -rf "$FFMPEG_SRC"
  if [[ -n "$checkout_parent" ]]; then
    rmdir "$checkout_parent"
  fi
fi
