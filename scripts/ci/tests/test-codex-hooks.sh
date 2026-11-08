#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for .codex/hooks/*.sh and .claude/hooks/*.sh: a formatter that fails is
# reported on stderr and never blocks the edit; the unsafe-command hook denies
# a forced push to master, direct Meson and ninja test runner invocations, and
# allows harmless commands, the run_meson_test.py wrapper, a ninja build
# followed by a separate command on the next line, and a doc path mention.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../.." && pwd)"
codex_hooks="$repo_root/.codex/hooks"
claude_hooks="$repo_root/.claude/hooks"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
printf '#!/bin/sh\nexit 1\n' >"$tmp/bin/black"
chmod +x "$tmp/bin/black"
printf 'x = 1\n' >"$tmp/a.py"

# The forbidden commands are assembled at run time so this file never carries
# the literal text the repository's own command guards match on.
force="--for""ce"
forced_push="git push $force origin master"

m="me""son"
t="te""st"
n="nin""ja"
c="com""pile"
nl=$'\n'

refused_env_cases=(
  "$m $t -C build"
  "cd core && $m $t --suite=fast"
  "cd x && bash -c '$m $t -C build'"
  "env FOO=1 $m $t -C build"
  "timeout 600  $m  $t -C build"
  "praetorctl admit --label x -- $m $t -C build"
  "$n -C build $t"
  "$n -C build -j8 $t"
  "$m $c -C build $t"
)

allowed_env_cases=(
  "python3 scripts/ci/run_meson_test.py -- -C build"
  "python3 scripts/ci/run_meson_test.py -- --suite=fast test_name"
  "$n -C build $t/test_model"
  "$n -C build"
  "$m setup build"
  "$m introspect --tests"
  "make $t"
  "ls -la"
  "$n -C build${nl}$t -f build/core/vmaf"
  "cat docs/development/$m-$t-runner.md"
)

fail() {
  echo "FAIL $1" >&2
  exit 1
}

# 1. Formatter test (Codex hook)
rc=0
err=$(PATH="$tmp/bin:$PATH" CLAUDE_TOOL_INPUT_file_path="$tmp/a.py" bash "$codex_hooks/auto-format-on-edit.sh" 2>&1 </dev/null) || rc=$?
[ "$rc" -eq 0 ] || fail "failing formatter blocked the edit (rc=$rc)"
case "$err" in *"black failed on"*) ;; *) fail "failing formatter not reported: $err" ;; esac
echo "ok   failing formatter is reported, edit not blocked"

# 2. Forced push tests on Codex hook
rc=0
CLAUDE_TOOL_INPUT_command="$forced_push" bash "$codex_hooks/block-unsafe-bash.sh" >/dev/null 2>&1 </dev/null || rc=$?
[ "$rc" -eq 2 ] || fail "forced push to master not denied (rc=$rc)"
echo "ok   forced push denied (rc=2)"

rc=0
CLAUDE_TOOL_INPUT_command="ls -la" bash "$codex_hooks/block-unsafe-bash.sh" >/dev/null 2>&1 </dev/null || rc=$?
[ "$rc" -eq 0 ] || fail "harmless command denied (rc=$rc)"
echo "ok   harmless command allowed (rc=0)"

rc=0
printf '{"tool_input":{"command":"%s"}}' "$forced_push" | bash "$codex_hooks/block-unsafe-bash.sh" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] || fail "stdin JSON forced push not denied (rc=$rc)"
echo "ok   stdin JSON forced push denied (rc=2)"

# 3. Direct test-runner cases against BOTH hook files
hook_scripts=(
  "$codex_hooks/block-unsafe-bash.sh"
  "$claude_hooks/block-unsafe-bash.sh"
)

for hook in "${hook_scripts[@]}"; do
  hook_name="${hook#"$repo_root/"}"

  # Refused cases via CLAUDE_TOOL_INPUT_command (rc 2, stderr names run_meson_test.py)
  for cmd in "${refused_env_cases[@]}"; do
    rc=0
    err=$(CLAUDE_TOOL_INPUT_command="$cmd" bash "$hook" 2>&1 </dev/null) || rc=$?
    [ "$rc" -eq 2 ] || fail "refused command '$cmd' returned rc=$rc on $hook_name (expected 2)"
    case "$err" in
      *"scripts/ci/run_meson_test.py"*) ;;
      *) fail "refused command '$cmd' stderr did not name scripts/ci/run_meson_test.py on $hook_name: $err" ;;
    esac
    echo "ok   refused (rc=$rc, $hook_name): $cmd"
  done

  # Refused case: JSON-on-stdin form of the first case
  first_case="$m $t -C build"
  rc=0
  err=$(printf '{"tool_input":{"command":"%s"}}' "$first_case" | bash "$hook" 2>&1) || rc=$?
  [ "$rc" -eq 2 ] || fail "stdin JSON refused command returned rc=$rc on $hook_name (expected 2)"
  case "$err" in
    *"scripts/ci/run_meson_test.py"*) ;;
    *) fail "stdin JSON refused command stderr did not name scripts/ci/run_meson_test.py on $hook_name: $err" ;;
  esac
  echo "ok   refused (rc=$rc, $hook_name, stdin JSON): $first_case"

  # Allowed cases (rc 0)
  for cmd in "${allowed_env_cases[@]}"; do
    rc=0
    err=$(CLAUDE_TOOL_INPUT_command="$cmd" bash "$hook" 2>&1 </dev/null) || rc=$?
    [ "$rc" -eq 0 ] || fail "allowed command '$cmd' returned rc=$rc on $hook_name (expected 0): $err"
    echo "ok   allowed (rc=$rc, $hook_name): $cmd"
  done
done
