# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# shellcheck shell=bash

# scripts/ci/pr-body-input.sh — shared stdin classification for the two
# ADR-0108 PR-body entry points, `scripts/ci/deliverables-check.sh` and
# `scripts/ci/validate-pr-body.sh`. Sourced, never executed.
#
# Why this exists
# ---------------
# Both scripts used to choose their input with `[ ! -t 0 ]`. That test
# answers "is fd 0 something other than a terminal", which is a different
# question from "did somebody pipe me a PR body". It is true for a pipe
# carrying a body — and equally true for `</dev/null`, for a CI step whose
# stdin is a null device, and for a descriptor the caller has closed.
#
# The closed-descriptor case was the sharp edge: `PR_BODY="$(cat)"` with
# fd 0 closed does not fail, it **deadlocks**. Command substitution opens a
# pipe, the kernel hands out the lowest free descriptor, and with fd 0 free
# that pipe's read end lands on fd 0 itself. `cat` then reads the pipe it is
# writing to, no writer ever closes, and the gate hangs until something
# external kills it. Reproduce with:
#
#   timeout 10 bash scripts/ci/deliverables-check.sh 0<&- ; echo $?   # 124
#
# Duplicating fd 0 up front is what makes the rest safe. The duplicate is a
# descriptor no later command substitution can steal, so the classification
# below inspects the caller's real stdin and reads from it directly.

# pr_body_classify_stdin — decide what fd 0 actually is, consuming nothing.
#
# Sets PR_BODY_STDIN_KIND to exactly one of:
#   stream      a pipe, regular file or socket — a body can be read from it
#   tty         the terminal — the caller is interactive and piped nothing
#   unreadable  /dev/null or another descriptor that cannot carry a body
#   closed      fd 0 is closed; reading it is the deadlock described above
#
# For "stream" it also sets PR_BODY_STDIN_FD to a private duplicate of fd 0;
# pass that to pr_body_read_stdin, then release it with pr_body_close_stdin.
# For every other kind PR_BODY_STDIN_FD is empty and nothing needs releasing —
# the caller decides what an absent body means for its own gate.
pr_body_classify_stdin() {
  local fd=""

  PR_BODY_STDIN_KIND=""
  PR_BODY_STDIN_FD=""

  if [ -t 0 ]; then
    PR_BODY_STDIN_KIND="tty"
    return 0
  fi

  # The brace group keeps the `2>/dev/null` scoped to the probe: a bare
  # `exec ... 2>/dev/null` would redirect this shell's stderr permanently.
  if ! { exec {fd}<&0; } 2>/dev/null; then
    PR_BODY_STDIN_KIND="closed"
    return 0
  fi

  # Only refuse a descriptor we can positively identify as a non-stream.
  # Where /dev/fd is not mounted nothing is identifiable, so the descriptor
  # is read as before — the deadlock is already ruled out by the dup above.
  if [ -e "/dev/fd/${fd}" ] &&
    [ ! -p "/dev/fd/${fd}" ] &&
    [ ! -f "/dev/fd/${fd}" ] &&
    [ ! -S "/dev/fd/${fd}" ]; then
    exec {fd}<&-
    PR_BODY_STDIN_KIND="unreadable"
    return 0
  fi

  PR_BODY_STDIN_FD="${fd}"
  PR_BODY_STDIN_KIND="stream"
}

# pr_body_read_stdin — print everything the classified stream holds.
#
# Call only after pr_body_classify_stdin set PR_BODY_STDIN_KIND to "stream".
# An empty result means the producer sent nothing, which is a fact about the
# producer and is reported as such by the caller — not silently parsed as a
# PR description with all six deliverables missing.
pr_body_read_stdin() {
  if [ "${PR_BODY_STDIN_KIND:-}" != "stream" ] || [ -z "${PR_BODY_STDIN_FD:-}" ]; then
    echo "pr_body_read_stdin: called without a classified stdin stream" >&2
    return 2
  fi
  cat <&"${PR_BODY_STDIN_FD}"
}

# pr_body_close_stdin — release the duplicate pr_body_classify_stdin handed out.
#
# Call it from the caller's own shell once the body has been read. It cannot be
# folded into pr_body_read_stdin: that function runs inside `$( )`, and an
# `exec {fd}<&-` there closes the *subshell's* copy while the caller's stays
# open. Leaving it open is not fatal, but it is a descriptor the gate never
# uses again and every process it spawns afterwards inherits — `git`,
# `python3`, `mktemp` in these scripts all receive it — and a second
# classification would strand another one.
#
# Safe to call when nothing was duplicated: for "tty", "closed" and
# "unreadable" PR_BODY_STDIN_FD is empty and this is a no-op.
pr_body_close_stdin() {
  if [ -n "${PR_BODY_STDIN_FD:-}" ]; then
    exec {PR_BODY_STDIN_FD}<&-
  fi
  PR_BODY_STDIN_FD=""
}

# pr_body_stdin_reason — one line naming why fd 0 cannot supply a body.
pr_body_stdin_reason() {
  case "${PR_BODY_STDIN_KIND:-}" in
    tty)
      echo "stdin is a terminal, so nothing was piped."
      ;;
    closed)
      echo "stdin (fd 0) is closed, so nothing can be read from it."
      ;;
    unreadable)
      echo "stdin is not a readable stream (/dev/null or a similar descriptor)."
      ;;
    *)
      echo "stdin carries no PR body."
      ;;
  esac
}
