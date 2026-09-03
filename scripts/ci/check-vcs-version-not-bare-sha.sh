#!/usr/bin/env bash
# Guard VMAF_VERSION against silently degrading to a bare commit abbreviation.
#
# core/include/meson.build derives VMAF_VERSION from
#   git describe --tags --long --match 'v*.*.*'
# and meson substitutes the vcs_tag `fallback` whenever that command fails
# (mesonbuild/scripts/vcstagger.py catches any exception from the subprocess).
#
# Adding `--always` breaks that contract: git then exits 0 even with no
# reachable tag and prints a bare abbreviated object name, which becomes
# VMAF_VERSION verbatim. Every version surface — `vmaf --version`, the JSON and
# XML `version` field, vmaf_version(), the pkg-config metadata consumers read —
# then reports something like "abafdfc" instead of a version, on any shallow CI
# checkout, tarball export, or worktree whose .git is a file.
#
# The failure is silent and intermittent rather than deterministic: it only
# becomes *visible* when the seven-character abbreviation happens to contain no
# ASCII digit, which core/test/test_output.c::test_vmaf_version asserts against.
# That is roughly one commit in a thousand — (6/16)^7 — so the defect can sit in
# tree for months and then fail an unrelated PR. It did: merge commit
# abafdfcc3c8ef40c369b4bb776c14188729ceada abbreviates to "abafdfc".
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

target=core/include/meson.build
fail=0

note() { printf '%s\n' "$*" >&2; }
bad() {
  note "error: $*"
  fail=1
}

if [ ! -f "$target" ]; then
  note "error: $target not found; cannot verify the version-string contract"
  exit 1
fi

# Isolate the vcs_tag(...) call so a stray '--always' elsewhere in the file
# (a comment explaining this very rule, for instance) is not mistaken for one
# inside the command array.
call=$(awk '
  /vcs_tag\(/           { depth = 1; buf = $0; next }
  depth > 0 {
    buf = buf "\n" $0
    n = gsub(/\(/, "(") ; depth += n
    n = gsub(/\)/, ")") ; depth -= n
    if (depth <= 0) { print buf; exit }
  }
' "$target")

if [ -z "$call" ]; then
  bad "$target contains no vcs_tag(...) call; VMAF_VERSION generation moved?"
  note "       If generation legitimately moved, update this gate to match."
  exit 1
fi

# Strip comments before matching so prose may discuss the banned flag freely.
code=$(printf '%s\n' "$call" | sed 's/#.*$//')

if printf '%s\n' "$code" | grep -q -- "--always"; then
  bad "$target passes --always to git describe."
  note "       With --always, a checkout that cannot reach a v*.*.* tag still"
  note "       exits 0 and yields a bare commit abbreviation, which becomes"
  note "       VMAF_VERSION verbatim. Drop --always so git fails and meson"
  note "       substitutes the fallback version instead."
fi

if ! printf '%s\n' "$code" | grep -qE '(^|[[:space:],])fallback[[:space:]]*:'; then
  bad "$target does not set an explicit vcs_tag fallback."
  note "       meson defaults it to meson.project_version(), but the fallback is"
  note "       load-bearing here: it is the entire tagless-checkout path. Spell"
  note "       it out so the intent survives a meson upgrade."
fi

if ! printf '%s\n' "$code" | grep -q -- "--match"; then
  bad "$target no longer restricts git describe with --match."
  note "       Without --match, any tag in the repository can supply the version."
fi

if [ "$fail" -ne 0 ]; then
  note ""
  note "See core/include/meson.build and core/test/test_output.c::test_vmaf_version."
  exit 1
fi

printf 'check-vcs-version-not-bare-sha: OK (%s keeps VMAF_VERSION a real version)\n' "$target"
