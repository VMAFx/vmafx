#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# Unpatched upstream GStreamer `vmaf` plugin (gst-plugins-bad at
# GST_PLUGINS_BAD_VERSION from build-config.env) built against a libvmaf
# install, run on two raw files, scores compared as exact text. Proves the
# compat libvmaf keeps source and binary compatibility for consumers that know
# nothing about this fork. See docs/development/upstream-consumers.md.
set -euo pipefail

# shellcheck source=scripts/ci/upstream-consumer-lib.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/upstream-consumer-lib.sh"
# The upstream clone goes into a temporary directory. Run from a git hook, it would write
# into the caller's repository through the hook's GIT_DIR.
# shellcheck source=scripts/lib/drop-git-env.sh
. "$UC_REPO/scripts/lib/drop-git-env.sh"

uc_defaults
uc_parse_common "$@"
if [ "${#UC_REST[@]}" -gt 0 ]; then
  case "${UC_REST[0]}" in
    -h | --help)
      uc_usage "upstream-gstreamer-compat.sh" \
        "  --cuda                  accepted for symmetry; the upstream element has no CUDA path, so
                          the run prints a SKIP line"
      exit 0
      ;;
    *) uc_die "unknown argument: ${UC_REST[0]}" ;;
  esac
fi
uc_validate

for tool in git meson ninja pkg-config gst-launch-1.0 gst-inspect-1.0; do
  command -v "$tool" >/dev/null 2>&1 || uc_die "missing tool: $tool"
done

GST_VER="$(uc_cfg GST_PLUGINS_BAD_VERSION)"
GST_REMOTE="$(uc_cfg GST_PLUGINS_BAD_REMOTE)"
GST_ROOT="$WORK/gst-plugins-bad-$GST_VER"
GST_SRC="$GST_ROOT/src"
GST_BUILD="$GST_ROOT/build"
GST_PLUGDIR="$GST_ROOT/plugins"
GST_SUBDIR="subprojects/gst-plugins-bad"
STATUS=0

# Extra pkg-config directories searched AFTER the libvmaf prefix (for example a
# glib-2.0.pc that names a glib-mkenums when the distribution splits it out).
EXTRA_PKG="${UPSTREAM_CONSUMER_EXTRA_PKG_CONFIG_PATH:-}"

gst_pkgpath() {
  local p
  p="$(uc_pkgpath "$PREFIX")"
  if [ -n "$EXTRA_PKG" ]; then
    p="$p:$EXTRA_PKG"
  fi
  echo "$p"
}

gst_check_versions() {
  local have
  have="$(pkg-config --modversion gstreamer-1.0 2>/dev/null)" ||
    uc_die "gstreamer-1.0 development files not found by pkg-config"
  echo "== GStreamer runtime: $(gst-launch-1.0 --version | head -n 1) (pkg-config $have)"
  [ "${have%.*}" = "${GST_VER%.*}" ] ||
    uc_die "installed GStreamer $have and pinned gst-plugins-bad $GST_VER differ in major.minor"
  PKG_CONFIG_PATH="$(gst_pkgpath)" pkg-config --exists libvmaf ||
    uc_die "pkg-config finds no libvmaf under $PREFIX"
  local mk
  mk="$(PKG_CONFIG_PATH="$(gst_pkgpath)" pkg-config --variable=glib_mkenums glib-2.0 2>/dev/null)" ||
    uc_die "glib-2.0 development files not found by pkg-config"
  [ -x "$mk" ] ||
    uc_die "glib-mkenums not found (install the glib development package, or set UPSTREAM_CONSUMER_EXTRA_PKG_CONFIG_PATH to a dir with a glib-2.0.pc that names one)"
}

# Sparse checkout: only subprojects/gst-plugins-bad of the monorepo tag.
gst_fetch() {
  if [ -d "$GST_SRC/.git" ] &&
    [ "$(uc_exact_tag "$GST_SRC")" = "$GST_VER" ] &&
    [ -f "$GST_SRC/$GST_SUBDIR/ext/vmaf/meson.build" ]; then
    uc_log "reusing gst-plugins-bad $GST_VER at $GST_SRC"
    return 0
  fi
  rm -rf "$GST_SRC"
  mkdir -p "$GST_ROOT"
  uc_log "sparse-cloning $GST_REMOTE at $GST_VER"
  timeout 600 git -c advice.detachedHead=false clone --quiet --depth 1 \
    --filter=blob:none --sparse --branch "$GST_VER" "$GST_REMOTE" "$GST_SRC" ||
    uc_die "cannot clone GStreamer $GST_VER"
  timeout 600 git -C "$GST_SRC" sparse-checkout set "$GST_SUBDIR" ||
    uc_die "sparse checkout of $GST_SUBDIR failed"
  [ -f "$GST_SRC/$GST_SUBDIR/ext/vmaf/meson.build" ] || uc_die "ext/vmaf missing from $GST_VER"
}

# Build ONLY the vmaf plugin; reconfigure when the prefix or pin changes.
gst_build() {
  local stamp="$GST_BUILD/.compat-stamp" want="$PREFIX|$GST_VER"
  if [ -f "$GST_BUILD/ext/vmaf/libgstvmaf.so" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
    uc_log "reusing plugin build $GST_BUILD"
    return 0
  fi
  rm -rf "$GST_BUILD"
  uc_log "configuring the vmaf plugin against $PREFIX"
  PKG_CONFIG_PATH="$(gst_pkgpath)" timeout 300 meson setup "$GST_BUILD" "$GST_SRC/$GST_SUBDIR" \
    -Dauto_features=disabled -Dvmaf=enabled -Dtests=disabled -Dexamples=disabled \
    -Ddoc=disabled -Dintrospection=disabled -Dgpl=disabled -Dnls=disabled \
    >"$GST_ROOT/meson-setup.log" 2>&1 || uc_die "meson setup failed (see $GST_ROOT/meson-setup.log)"
  nice -n 10 timeout 590 ninja -j"$JOBS" -C "$GST_BUILD" ext/vmaf/libgstvmaf.so \
    >"$GST_ROOT/ninja.log" 2>&1 || uc_die "plugin build failed (see $GST_ROOT/ninja.log)"
  printf '%s' "$want" >"$stamp"
}

# gst_env PREFIX CMD...: run CMD with ONLY our plugin plus the core and raw
# parser plugins of the system; the system's own vmaf plugin is never on the
# path. LD_DEBUG=libs makes the dynamic loader name every library it runs.
gst_env() {
  local prefix="$1"
  shift
  env LD_LIBRARY_PATH="$(uc_ldpath "$prefix")" LD_DEBUG=libs \
    GST_PLUGIN_SYSTEM_PATH_1_0="" GST_PLUGIN_PATH_1_0="$GST_PLUGDIR" GST_PLUGIN_PATH="" \
    GST_REGISTRY_1_0="$GST_ROOT/registry-$$.bin" GST_REGISTRY_FORK=no \
    "$@"
}

# The system plugins the pipeline needs (found by name, never by guess).
gst_plugdir() {
  local elem file
  rm -rf "$GST_PLUGDIR"
  mkdir -p "$GST_PLUGDIR"
  for elem in filesrc rawvideoparse fakesink; do
    file="$(gst-inspect-1.0 "$elem" 2>/dev/null | sed -n 's/^ *Filename *//p' | head -n 1)"
    [ -f "$file" ] || uc_die "cannot locate the system plugin of element $elem"
    ln -sf "$file" "$GST_PLUGDIR/$(basename "$file")"
  done
  ln -sf "$GST_BUILD/ext/vmaf/libgstvmaf.so" "$GST_PLUGDIR/libgstvmaf.so"
}

# Name the plugin that would run, and require it to be the one we built.
gst_check_plugin() {
  local info file
  info="$(gst_env "$PREFIX" gst-inspect-1.0 vmaf 2>"$GST_ROOT/inspect.err")" ||
    uc_die "gst-inspect-1.0 finds no vmaf element (see $GST_ROOT/inspect.err)"
  file="$(printf '%s\n' "$info" | sed -n 's/^ *Filename *//p' | head -n 1)"
  echo "== GST_PLUGIN_PATH_1_0=$GST_PLUGDIR (GST_PLUGIN_SYSTEM_PATH_1_0 empty)"
  echo "== gst-inspect-1.0 vmaf Filename: $file"
  [ "$file" = "$GST_PLUGDIR/libgstvmaf.so" ] &&
    [ "$(readlink -f "$file")" = "$(readlink -f "$GST_BUILD/ext/vmaf/libgstvmaf.so")" ] ||
    uc_die "the vmaf plugin found is not the one built from $GST_VER"
  printf '%s\n' "$info" | grep -q 'results-filename' || uc_die "built plugin lacks results-filename"
}

# gst_score PREFIX OUT_JSON: N frames through two filesrc/rawvideoparse chains
# into the element. filesrc emits exactly FRAMES one-frame buffers, so EOS
# comes at the same frame on every run.
gst_score() {
  local prefix="$1" out="$2" log frame_bytes
  log="$out.run.log"
  frame_bytes=$((SIZE_W * SIZE_H * 3 / 2))
  rm -f "$out"
  gst_env "$prefix" timeout 300 gst-launch-1.0 -e \
    vmaf name=v model-filename=vmaf_v0.6.1 threads=1 results-format=json results-filename="$out" \
    ! fakesink \
    filesrc location="$REF_YUV" blocksize="$frame_bytes" num-buffers="$FRAMES" \
    ! rawvideoparse format=i420 width="$SIZE_W" height="$SIZE_H" framerate=25/1 ! v.ref_sink \
    filesrc location="$DIST_YUV" blocksize="$frame_bytes" num-buffers="$FRAMES" \
    ! rawvideoparse format=i420 width="$SIZE_W" height="$SIZE_H" framerate=25/1 ! v.dist_sink \
    >"$log" 2>&1 || uc_die "gst-launch-1.0 failed (see $log)"
  [ -s "$out" ] || uc_die "the vmaf element wrote no score file $out (see $log)"
  uc_check_loaded "$prefix" "$log"
}

# gst_compare A B LABEL_A LABEL_B RENAME_B: A is always a GStreamer file; B is
# one too when RENAME_B is 1. The element names its model "self"
# (gstvmafelement.c: model_cfg.name = "self"), so its score is called `self`
# where libvmaf's own output says `vmaf`; that one name is mapped, every value
# is still compared as exact text.
gst_compare() {
  local rc=0 rename_b=()
  if [ "$5" = 1 ]; then
    rename_b=(--rename-b self=vmaf)
  fi
  uc_compare "$1" "$2" "$3" "$4" --rename-a self=vmaf "${rename_b[@]}" || rc=$?
  case "$rc" in
    0) ;;
    1) STATUS=1 ;;
    *) exit "$UC_EXIT_SETUP" ;;
  esac
}

gst_check_versions
gst_fetch
gst_build
gst_plugdir
gst_check_plugin

out="$WORK/gst-prefix.json"
echo "== scoring with --prefix $PREFIX"
gst_score "$PREFIX" "$out"
if [ -n "$REF_PREFIX" ]; then
  echo "== re-running the same plugin binary against --reference-prefix $REF_PREFIX"
  gst_score "$REF_PREFIX" "$WORK/gst-reference.json"
  gst_compare "$out" "$WORK/gst-reference.json" "gst+prefix" "gst+reference-prefix" 1
fi
if [ "$AGAINST_CLI" -eq 1 ]; then
  echo "== comparing with the vmaf CLI of --prefix"
  uc_cli_scores "$PREFIX" "$WORK/cli-prefix-gst.json"
  gst_compare "$out" "$WORK/cli-prefix-gst.json" "gst" "vmaf-cli" 0
fi
if [ "$CUDA" -eq 1 ]; then
  echo "SKIP: upstream GStreamer vmaf element has no CUDA path"
fi

if [ "$STATUS" -eq 0 ]; then
  echo "PASS: upstream gst-plugins-bad $GST_VER vmaf scores match"
else
  echo "FAIL: upstream gst-plugins-bad $GST_VER vmaf scores differ"
fi
exit "$STATUS"
