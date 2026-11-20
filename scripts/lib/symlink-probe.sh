# shellcheck shell=bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Probe whether a directory accepts symbolic links before a shell test fixture
# creates one. On Windows volumes or filter drivers that reject symbolic links,
# hosts without the symbolic-link privilege, or environments where `ln -s`
# creates no symbolic link, `symlink_probe` prints the reason and returns 1 so
# the caller skips the case with a stated reason (HISS-21).

symlink_probe() {
  local dir="$1"
  local probe="$dir/.symlink-probe.$$"
  local out=""
  local rc=0
  out="$(ln -s symlink-probe-target "$probe" 2>&1)" || rc=$?
  if [ "$rc" -eq 0 ] && [ -L "$probe" ]; then
    rm -f -- "$probe"
    return 0
  fi
  rm -f -- "$probe"
  if [ "$rc" -eq 0 ]; then
    printf '%s\n' "ln -s created no symbolic link"
  else
    printf '%s\n' "$out"
  fi
  return 1
}
