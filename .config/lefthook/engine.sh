#!/bin/sh
# The one place a Praetor Git hook chooses which engine judges the repository (#906).
# Every governance job in lefthook.yml and the fallback pre-commit hook run
# "sh .config/lefthook/engine.sh <praetorctl arguments>" from the repository root.
#
# The engine is the one the repository pins, not the newest binary on PATH. The pin is the
# value of PRAETOR_REF declared in a workflow under .github/workflows ("PRAETOR_REF: <ref>" or
# "PRAETOR_REF=<ref>"), or the uses ref of praetor-adopt / reusable workflows
# ("uses: cordanaLLM/praetor/.github/actions/praetor-adopt@<ref>" or
# "uses: cordanaLLM/praetor/.github/workflows/<workflow>@<ref>"), the same declaration a hosted
# Standards job installs the engine from with "go install <module>/cmd/standardsctl@${PRAETOR_REF}".
# A pin is a commit id of 7 to 40 hexadecimal digits or a release tag such as v1.2.3; a branch
# name moves, so it is no pin. A non-pin uses ref such as @main is treated as unpinned (running
# the engine on PATH).
#
# With a pin, the engine is installed once per pin into
#   ${XDG_CACHE_HOME:-$HOME/.cache}/praetor/engine/<pin>
# by "go install", bounded by PRAETOR_ENGINE_INSTALL_TIMEOUT seconds (default 300), and run
# from there. A pin that cannot be read, is declared twice with different values, or cannot be
# installed fails the hook with a message naming the pin, the cache path and the command that
# fixes it; the hook never falls back to the binary on PATH, which may be a different version.
#
# With no pin the hook runs the binary on PATH, praetorctl first and then the older name
# standardsctl, and says so on standard error.
#
# Every line below ends in a comment sign, blank ones included. praetorctl adopt writes this
# file into repositories whose checkout may give it CRLF line endings (core.autocrlf), and a
# shell reads the carriage return as part of the last word of a line. Behind a comment sign
# it is part of the comment.
set -eu #
module=github.com/cordanaLLM/praetor #
package=cmd/standardsctl #
binary=standardsctl #
#
# refuse MESSAGE: the hook fails closed with one line.
refuse() { #
  echo "praetor hooks: $*" >&2 #
  exit 1 #
} #
#
timeout=${PRAETOR_ENGINE_INSTALL_TIMEOUT:-300} #
case "$timeout" in #
  *[!0-9]* | 0 | 00*) refuse "PRAETOR_ENGINE_INSTALL_TIMEOUT='$timeout' must be an integer >= 1" ;; #
esac #
#
# pin_fault REF: succeeds, printing why, when REF is no commit id or release tag.
pin_fault() { #
  case "$1" in #
    *[!0-9a-fA-F]*) ;; #
    ???????*) [ "${#1}" -le 40 ] && return 1 ;; #
  esac #
  case "$1" in #
    v[0-9]*.[0-9]*.[0-9]*) #
      major=${1#v} #
      major=${major%%.*} #
      case "$major" in *[!0-9]*) ;; *) #
        rem=${1#v"$major".} #
        minor=${rem%%.*} #
        case "$minor" in *[!0-9]*) ;; *) #
          rem=${rem#"$minor".} #
          patch_digits=${rem%%[!0-9]*} #
          if [ -n "$patch_digits" ]; then #
            suffix=${rem#"$patch_digits"} #
            case "$suffix" in #
              "") return 1 ;; #
              [-+]*) #
                case "$suffix" in #
                  *[!A-Za-z0-9._+-]*) ;; #
                  *) return 1 ;; #
                esac #
                ;; #
            esac #
          fi #
          ;; #
        esac #
        ;; #
      esac #
      ;; #
  esac #
  echo "it is neither a commit id of 7 to 40 hexadecimal digits nor a release tag such as v1.2.3" #
} #
#
# declared_pins: one "file<TAB>value" line per PRAETOR_REF declaration under .github/workflows.
declared_pins() { #
  for file in .github/workflows/*.yml .github/workflows/*.yaml; do #
    [ -e "$file" ] || continue #
    [ -r "$file" ] || refuse "workflow file $file is not readable" #
    tr -d '\r' <"$file" | sed -n 's/^[[:space:]]*PRAETOR_REF[[:space:]]*[:=][[:space:]]*//p' | sed -e 's/[[:space:]]*#.*$//' -e "s/^[\"']//" -e "s/[\"'][[:space:]]*\$//" -e 's/[[:space:]]*$//' | while read -r value; do printf '%s\t%s\n' "$file" "$value"; done #
  done #
} #
#
# uses_pins: one "file<TAB>value" line per praetor-adopt or reusable-workflow uses ref under .github/workflows.
uses_pins() { #
  for file in .github/workflows/*.yml .github/workflows/*.yaml; do #
    [ -e "$file" ] || continue #
    [ -r "$file" ] || refuse "workflow file $file is not readable" #
    tr -d '\r' <"$file" | sed -n -e 's/^[[:space:]]*-[[:space:]]*uses:[[:space:]]*//p' -e 's/^[[:space:]]*uses:[[:space:]]*//p' | sed -e 's/[[:space:]]*#.*$//' -e "s/^[\"']//" -e "s/[\"'][[:space:]]*\$//" -e 's/[[:space:]]*$//' | sed -n -e 's/^[Cc][Oo][Rr][Dd][Aa][Nn][Aa][Ll][Ll][Mm]\/[Pp][Rr][Aa][Ee][Tt][Oo][Rr]\/\.github\/actions\/praetor-adopt@\(..*\)$/\1/p' -e 's/^[Cc][Oo][Rr][Dd][Aa][Nn][Aa][Ll][Ll][Mm]\/[Pp][Rr][Aa][Ee][Tt][Oo][Rr]\/\.github\/workflows\/[^@[:space:]]*@\(..*\)$/\1/p' | while read -r value; do #
      if fault=$(pin_fault "$value"); then :; else printf '%s\t%s\n' "$file" "$value"; fi #
    done #
  done #
} #
#
# run_pinned REF FILE ARGUMENT...: run the engine pinned to REF, declared in FILE, with the
# arguments, installing it first when it is not cached.
run_pinned() { #
  pin=$1 #
  source=$2 #
  shift 2 #
  cache=${XDG_CACHE_HOME:-${HOME:-}/.cache}/praetor/engine #
  directory=$cache/$pin #
  fix="GOBIN=$directory go install $module/$package@$pin" #
  where="PRAETOR_REF=$pin (declared in $source), engine cache $directory" #
  case "$cache" in /.cache/*) refuse "cannot place the engine cache for $where: neither XDG_CACHE_HOME nor HOME is set; set one and run: $fix" ;; esac #
  if [ ! -x "$directory/$binary" ] && [ ! -x "$directory/$binary.exe" ]; then #
    install_pinned "$directory" "$where" "$fix" #
  fi #
  if [ "${1:-}" = "--print-path" ]; then #
    if [ -x "$directory/$binary" ]; then printf '%s\n' "$directory/$binary"; exit 0; fi #
    printf '%s\n' "$directory/$binary.exe"; exit 0 #
  fi #
  if [ -x "$directory/$binary" ]; then exec "$directory/$binary" "$@"; fi #
  exec "$directory/$binary.exe" "$@" #
} #
#
# install_pinned DIRECTORY WHERE FIX: go install the pinned engine into DIRECTORY, bounded by
# the timeout, through a scratch directory whose files are renamed into place so an interrupted
# install leaves no half-written engine under the pin.
install_pinned() { #
  command -v go >/dev/null 2>&1 || refuse "the Go toolchain is not on PATH, so the engine pinned by $2 cannot be installed; install Go and run: $3" #
  scratch=$1.tmp.$$ #
  rm -rf "$scratch" #
  mkdir -p "$scratch" || refuse "cannot create $scratch for the engine pinned by $2; run: $3" #
  echo "praetor hooks: installing the engine pinned by $2 (first use, at most $timeout seconds)" >&2 #
  # The installer writes to a log file, not to the hook's pipe: a compiler it started can outlive
  # the kill below, and one that holds the pipe would keep the hook waiting for it.
  log=$scratch.log #
  env GOFLAGS= GOWORK=off GOBIN="$scratch" go install "$module/$package@$pin" >"$log" 2>&1 </dev/null & #
  installer=$! #
  (sleep "$timeout" & sleeper=$!; trap 'kill "$sleeper" 2>/dev/null; exit 0' TERM INT; wait "$sleeper" 2>/dev/null; kill "$installer" 2>/dev/null) >/dev/null 2>&1 </dev/null & #
  watchdog=$! #
  status=0 #
  wait "$installer" || status=$? #
  if kill "$watchdog" 2>/dev/null; then watchdog='' ; fi #
  if [ "$status" -ne 0 ]; then #
    cat "$log" >&2 #
    rm -rf "$scratch" "$log" #
    refuse "installing the engine pinned by $2 failed (exit $status, timeout $timeout seconds); run: $3" #
  fi #
  rm -f "$log" #
  place_pinned "$1" "$2" "$3" #
} #
#
# place_pinned DIRECTORY WHERE FIX: move what install_pinned built into $scratch into DIRECTORY.
# Jobs that run in parallel, and worktrees that share the cache, each install on a cold cache.
# The target directory is never removed or replaced as a whole: each built file is renamed over
# the target one, which is atomic and swaps in an identical file when another job was first. A
# rename that fails because the engine is running (Windows) leaves that engine in place.
place_pinned() { #
  mkdir -p "$1" || refuse "cannot create $1 for the engine pinned by $2; run: $3" #
  for built in "$scratch"/*; do #
    if ! mv -f "$built" "$1/" 2>/dev/null && [ ! -e "$1/${built##*/}" ]; then #
      refuse "cannot move the installed engine into $1 for the pin in $2; run: $3" #
    fi #
  done #
  rm -rf "$scratch" #
  if [ ! -x "$1/$binary" ] && [ ! -x "$1/$binary.exe" ]; then #
    refuse "the installed engine is not in $1 for the pin in $2; run: $3" #
  fi #
} #
#
pins=$(declared_pins) #
[ -n "$pins" ] || pins=$(uses_pins) #
if [ -n "$pins" ]; then #
  values=$(printf '%s\n' "$pins" | cut -f2 | sort -u) #
  [ "$(printf '%s\n' "$values" | wc -l)" -eq 1 ] || refuse "the repository declares several PRAETOR_REF values, so no engine is pinned: $(printf '%s\n' "$pins" | tr '\t\n' '= ')" #
  pin=$values #
  file=$(printf '%s\n' "$pins" | cut -f1 | head -n 1) #
  if fault=$(pin_fault "$pin"); then #
    refuse "PRAETOR_REF='$pin' in $file is no usable pin: $fault" #
  fi #
  run_pinned "$pin" "$file" "$@" #
fi #
#
if path=$(command -v praetorctl 2>/dev/null || command -v standardsctl 2>/dev/null); then #
  if [ "${1:-}" = "--print-path" ]; then #
    printf '%s\n' "$path" #
    exit 0 #
  fi #
  echo "praetor hooks: no PRAETOR_REF pin under .github/workflows; running the engine on PATH: $path" >&2 #
  exec "$path" "$@" #
fi #
refuse "HISS governance hook cannot run because neither praetorctl nor standardsctl is installed" #
