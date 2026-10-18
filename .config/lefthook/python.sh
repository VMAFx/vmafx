#!/bin/sh
# The one place the hook policy starts its Python 3 interpreter (#339). Every job
# in praetor.yml and the pre-push script start their hook through this file, and
# a hook that starts another Python process reuses the interpreter it is running
# under (sys.executable), so no command spells an interpreter of its own.
#
# No variable selects the program: a value naming anything that exits 0 would
# pass every gate without running it. The candidates are a fixed list, tried in
# order, the same one the operator setting hooks.python defaults to
# (internal/config/operator_sections.go) and scripts/toolchain.py declares.
#
# A candidate is trusted only once it has proven itself: it runs the probe and
# must state Python 3 at or above the floor. A name on PATH proves nothing.
# Windows puts python.exe and python3.exe there as Microsoft Store aliases,
# which start, print "Python was not found" and exit nonzero. The first proven
# candidate replaces this shell, so its status is the hook's. With none left
# the hook fails as a missing dependency and names what was tried.
#
# Every line below ends in a comment sign, blank ones included. praetorctl adopt
# writes this file into repositories whose checkout may give it CRLF line
# endings (core.autocrlf), and a shell reads the carriage return as part of the
# last word of a line. Behind a comment sign it is part of the comment.
set -eu #
# Python 3.10: the release .config/hook-lint/requirements.txt is compiled for.
floor=10 #
probe='import sys; print(sys.version_info[0], sys.version_info[1], sep=chr(46))' #
# Python on Windows ends the line it prints with a carriage return.
cr=$(printf '\r') #
tried='' #
#
# Drop Git repository selection so nested git invocations (e.g. in test jobs)
# cannot target the pushing repository. Pre-commit hooks pass --keep-index to
# preserve GIT_INDEX_FILE for partial-commit staging.
keep_index=false #
if [ "${1-}" = "--keep-index" ]; then #
  keep_index=true #
  shift #
fi #
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES #
if [ "$keep_index" != true ]; then #
  unset GIT_INDEX_FILE #
fi #
#
# proven CANDIDATE [ARGUMENT]: it ran the probe and stated Python 3.floor or newer.
# The probe reads nothing: the hook's own standard input stays untouched.
proven() { #
  tried="${tried}${tried:+, }$*" #
  stated=$("$@" -c "$probe" 2>/dev/null </dev/null) || return 1 #
  stated=${stated%"$cr"} #
  case "$stated" in #
    3.*) minor=${stated#3.} ;; #
    *) return 1 ;; #
  esac #
  case "$minor" in #
    '' | *[!0-9]*) return 1 ;; #
  esac #
  [ "$minor" -ge "$floor" ] #
} #
#
if proven python3; then exec python3 "$@"; fi #
if proven python; then exec python "$@"; fi #
if proven py -3; then exec py -3 "$@"; fi #
echo "praetor hooks: missing dependency: no Python 3.$floor or newer interpreter." >&2 #
echo "Tried: $tried; none answered the version probe." >&2 #
echo "Install Python 3.$floor or newer under one of these names." >&2 #
exit 127 #
