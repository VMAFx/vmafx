#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# framework-hooks.sh — hand one lefthook stage to the pre-commit framework
# (ADR-1249, ADR-2012). lefthook.yml calls this file from a one-line `run:`.
#
# Why a file and not an inline `run: |` block: on Windows, lefthook v2.1.14
# starts `sh -c` through a hand-built command line and does not escape the
# script (internal/run/controller/exec/exec_windows.go builds
# `"<sh>" -c "<run>"`). The first double quote inside the script therefore
# ends the `-c` argument. The inline block failed with "unexpected end of file
# from `if' command", and a one-line command silently lost every word after
# its first quoted string. Keep double quotes out of every lefthook `run:`
# value; scripts/githooks/tests/test_install.py enforces that.
#
# Usage: framework-hooks.sh pre-commit
#        framework-hooks.sh pre-push REMOTE URL   (push refs on stdin)
set -euo pipefail

# Force UTF-8 on Windows where cp1252 is default, avoiding charmap decode errors
export PYTHONUTF8=1
export PYTHONIOENCODING=utf-8

find_framework() {
  # A repository virtualenv wins over PATH: bin/ is the POSIX layout and
  # Scripts/ the one `python -m venv` creates on Windows.
  local candidate
  for candidate in .venv/bin/pre-commit .venv/Scripts/pre-commit.exe; do
    if [ -f "$candidate" ] && [ -x "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  command -v pre-commit
}

if ! fw=$(find_framework); then
  echo "VMAFx hooks: pre-commit is missing; activate the environment used by make install-hooks." >&2
  exit 1
fi
case "$fw" in
  .venv/*)
    # Same effect as activating the virtualenv. The `language: system` hooks
    # it serves, such as reuse-lint, come from the same hash lock
    # (requirements/locks/pre-commit.txt), so they resolve from here too.
    tools="$PWD/${fw%/*}"
    if command -v cygpath >/dev/null 2>&1; then
      # Under lefthook, Git Bash reports PWD as C:/...; its colon would split
      # the PATH entry in two.
      tools=$(cygpath -u "$tools")
    fi
    # On Windows, python -m venv creates python.exe in Scripts/ but not python3.exe.
    # Provide python3.exe if missing so system hooks invoking python3 resolve to .venv.
    if [ -x "$tools/python.exe" ] && [ ! -e "$tools/python3.exe" ]; then
      cp "$tools/python.exe" "$tools/python3.exe"
    fi
    PATH="$tools:$PATH"
    export PATH
    ;;
esac

env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE -u GIT_OBJECT_DIRECTORY "$fw" install-hooks || exit 1

stage=${stage:-${1:-}}
case "$stage" in
  pre-commit)
    exec "$fw" run
    ;;
  pre-push)
    if [ "$#" -gt 0 ]; then
      shift
    fi
    # hook-impl needs Git's remote arguments and the push-ref stdin to compute
    # the pushed range (ADR-1241); exec keeps stdin attached.
    exec "$fw" hook-impl --config=.pre-commit-config.yaml --hook-type=pre-push \
      --hook-dir "$(git rev-parse --git-path hooks)" -- "$@"
    ;;
  *)
    echo "framework-hooks.sh: unknown stage '$stage'; expected pre-commit or pre-push" >&2
    exit 2
    ;;
esac
