#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/tests/test-pr-body-input-selection.sh — the ADR-0108 PR-body
# gates must decide what fd 0 is before reading it.
#
# Four gates read a PR body, and every one of them once chose its input with
# `[ ! -t 0 ]`, which answers "is fd 0 something other than a terminal" — not
# "did anybody pipe me a PR body". Three shapes fall in the gap, and this file
# pins all three for all four:
#
#   scripts/ci/deliverables-check.sh          ADR-0108 six-deliverable gate
#   scripts/ci/validate-pr-body.sh            its local mirror
#   scripts/ci/ffmpeg-patches-surface-check.sh  ADR-0409 surface gate
#   scripts/ci/state-md-touch-check.sh        ADR-0165 state.md gate
#
# The last two were fixed a commit later than the first two, which is the
# reason this file covers all four rather than the pair that happened to be
# noticed first: the defect was in a copied idiom, so a test naming only the
# scripts already fixed would have gone on passing while two gates still hung.
#
# The two pairs answer an absent body differently, and the expectations below
# encode that on purpose. deliverables-check.sh and validate-pr-body.sh exist
# only to parse a body, so no body is a usage error (exit 2). The surface and
# state.md gates also have a diff to check, so no body means the opt-out is
# unclaimable and they fall through to that diff — passing when it is clean,
# failing when it is not. Neither pair may hang.
#
# The shapes:
#
#   closed fd 0   `bash gate.sh 0<&-`. The old code ran `PR_BODY="$(cat)"`,
#                 the command substitution's pipe took the freed descriptor
#                 0, `cat` read the pipe it was writing to, and the gate
#                 HUNG — measured at the 12 s timeout below, unbounded in
#                 reality. Every case here is wrapped in `timeout` and a
#                 124 is reported as a hang, not merely as a wrong code.
#
# Exit 124 mostly means "hung" (issue #3457, 2026-11-15): under a parallel
# pre-push run the ffmpeg-patches gate can spend its whole 12 s budget in
# the #2435 invariant tree scans, which run before the PR-body stage in
# that gate, and die killed-but-working. The surface-gate cases below pass
# `--skip-invariants`, so their 12 s budget measures the fd-0 decision
# instead of the tree scans — the measured cause is out of the window. A
# 124 whose output carries the gate's `PR body from ...` line decided what
# fd 0 was and is re-run once with a 60 s budget, judged as usual; a slow
# case that completes with its expected verdict passes, a second timeout
# is HUNG — a hang after the decision is still a hang. The planted cases
# at the bottom keep both HUNG shapes armed, through the same run_case
# path the real cases take.
#
#   /dev/null     `bash gate.sh </dev/null`, the shape a CI step, a git
#                 hook and `nohup` all produce. Not a terminal, carries no
#                 body, and must be named as such.
#   empty pipe    an open pipe that carried no bytes: the producer sent
#                 nothing, and the gate must fail closed rather than parse
#                 an empty string into six missing deliverables.
#
# The positive controls in the same file are the point of the exercise: a
# real pipe, a real $PR_BODY and a real --body file must still be read.
#
# One more case sits below the gates, on the helper itself: the duplicate of
# fd 0 that pr_body_classify_stdin hands out has to be released once the body
# has been read, or every process the gate spawns afterwards inherits it.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
deliverables="${repo_root}/scripts/ci/deliverables-check.sh"
validator="${repo_root}/scripts/ci/validate-pr-body.sh"
ffmpeg_surface="${repo_root}/scripts/ci/ffmpeg-patches-surface-check.sh"
state_touch="${repo_root}/scripts/ci/state-md-touch-check.sh"
helper="${repo_root}/scripts/ci/pr-body-input.sh"

for script in "${deliverables}" "${validator}" "${ffmpeg_surface}" \
  "${state_touch}" "${helper}"; do
  if [ ! -r "${script}" ]; then
    echo "test-pr-body-input-selection: missing ${script}" >&2
    exit 1
  fi
done

if ! command -v timeout >/dev/null 2>&1; then
  echo "test-pr-body-input-selection: 'timeout' is required to bound the hang cases" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf -- "${work}"' EXIT

pass_count=0
fail_count=0

pass() {
  echo "PASS: $1"
  pass_count=$((pass_count + 1))
}

fail() {
  echo "FAIL: $1"
  fail_count=$((fail_count + 1))
}

# run_case <expected-exit|hung> <name> <shell-snippet>
#
# The snippet runs under `timeout 12 bash -c`, from the repo root, with
# PR_BODY unset unless the snippet sets it. Exit 124 is reported as a hang
# because that is the regression this file exists to catch. A 124 whose
# output carries the gate's `PR body from ...` line decided what fd 0 was
# inside the budget, so the kill landed on post-decision work or on load
# (issue #3457): the case is re-run once with a 60 s budget and judged as
# usual — a slow case that completes with its expected verdict passes, a
# second timeout is HUNG, because a hang after the decision is still a hang.
# expected="hung" is the planted negative control: the case must time out
# (either shape) and is reported as HUNG; anything else fails.
run_case() {
  local expected="$1" name="$2" snippet="$3"
  local out="" code=0 rerun=""
  out="$(cd "${repo_root}" && timeout 12 env -u PR_BODY bash -c "${snippet}" 2>&1)" || code=$?
  printf '%s' "${out}" >"${work}/last-output.txt"
  if [ "${code}" -eq 124 ] && grep -qiE -- 'PR body from' "${work}/last-output.txt"; then
    rerun="1"
    out="$(cd "${repo_root}" && timeout 60 env -u PR_BODY bash -c "${snippet}" 2>&1)" || code=$?
    printf '%s' "${out}" >"${work}/last-output.txt"
  fi
  if [ "${code}" -eq 124 ]; then
    if [ "${expected}" = "hung" ]; then
      if [ -n "${rerun}" ]; then
        pass "${name} — planted hang reported as HUNG (killed at 12 s with the decision line, again at 60 s)"
      else
        pass "${name} — planted hang reported as HUNG (killed at 12 s before the decision)"
      fi
    elif [ -n "${rerun}" ]; then
      fail "${name} — HUNG (killed again at 60 s after the PR-body decision); the gate must decide, not block"
    else
      fail "${name} — HUNG (killed at 12 s before the PR-body decision); the gate must decide, not block"
    fi
    return
  fi
  if [ "${expected}" = "hung" ]; then
    fail "${name} — the planted hang was expected to time out (got exit=${code})"
    echo "----- captured output -----"
    printf '%s\n' "${out}"
    echo "---------------------------"
    return
  fi
  if [ "${code}" -ne "${expected}" ]; then
    fail "${name} (got exit=${code}, expected ${expected})"
    echo "----- captured output -----"
    printf '%s\n' "${out}"
    echo "---------------------------"
    return
  fi
  pass "${name} (exit=${code})"
}

# expect_output <name> <grep-ere> — assert against the last run_case output.
expect_output() {
  local name="$1" pattern="$2"
  if grep -qiE -- "${pattern}" "${work}/last-output.txt"; then
    pass "${name}"
  else
    fail "${name}"
    echo "----- captured output -----"
    cat "${work}/last-output.txt"
    echo "---------------------------"
  fi
}

# expect_no_output <name> <grep-ere> — the last run_case must NOT have said this.
expect_no_output() {
  local name="$1" pattern="$2"
  if grep -qiE -- "${pattern}" "${work}/last-output.txt"; then
    fail "${name}"
    echo "----- captured output -----"
    cat "${work}/last-output.txt"
    echo "---------------------------"
  else
    pass "${name}"
  fi
}

# A body whose six deliverables are all opted out via ADR-0108 sentinels, so
# it parses clean against an empty diff. Used as the positive control: proof
# that a genuine stream still reaches the parser.
cat >"${work}/body.md" <<'EOF'
## Deep-dive deliverables

- [ ] **Research digest** — no digest needed: trivial.
- [ ] **Decision matrix** — no alternatives: only-one-way fix.
- [ ] **`AGENTS.md` invariant note** — no rebase-sensitive invariants.
- [ ] **Reproducer / smoke-test command** — no reproducer needed: docs-only.
- [ ] **CHANGELOG fragment** — no changelog needed: internal refactor.
- [ ] **Rebase note** — no rebase impact: docs-only.
EOF
: >"${work}/empty-diff.txt"

# `HEAD..HEAD` keeps deliverables-check.sh off the network and off
# origin/master: an empty diff, resolved from the env branch.
same_rev="BASE_SHA=HEAD HEAD_SHA=HEAD"

echo "--- scripts/ci/deliverables-check.sh ---"

run_case 2 "deliverables: closed fd 0 is a usage error, not a hang" \
  "bash '${deliverables}' 0<&-"
expect_output "deliverables: the closed-fd message names fd 0" \
  'stdin .fd 0. is closed'

run_case 2 "deliverables: </dev/null is a usage error" \
  "bash '${deliverables}' </dev/null"
expect_output "deliverables: the /dev/null message names the descriptor" \
  'not a readable stream'
expect_no_output "deliverables: /dev/null is not blamed on the PR description" \
  'description is empty'
expect_no_output "deliverables: /dev/null does not reach the six-item parser" \
  'missing deliverable'

run_case 1 "deliverables: an open pipe carrying no bytes fails closed" \
  "true | bash '${deliverables}'"
expect_output "deliverables: the empty-pipe message names the producer" \
  'carried no bytes'

run_case 1 "deliverables: a blank PR_BODY is attributed to the env var" \
  "PR_BODY='' ${same_rev} bash '${deliverables}'"
expect_output "deliverables: the blank-env message names PR_BODY" \
  'PR_BODY is set and blank'

run_case 0 "deliverables: a real pipe still reaches the parser" \
  "cat '${work}/body.md' | ${same_rev} bash '${deliverables}'"
expect_output "deliverables: the piped body reached the parser and passed" \
  'PASS — all six deliverables'

run_case 0 "deliverables: a file redirected onto stdin still reaches the parser" \
  "${same_rev} bash '${deliverables}' <'${work}/body.md'"

echo "--- scripts/ci/validate-pr-body.sh ---"

run_case 2 "validator: closed fd 0 is a usage error, not a hang" \
  "bash '${validator}' --diff '${work}/empty-diff.txt' 0<&-"
expect_output "validator: the closed-fd message names fd 0" \
  'stdin .fd 0. is closed'

run_case 2 "validator: </dev/null is a usage error" \
  "bash '${validator}' --diff '${work}/empty-diff.txt' </dev/null"
expect_output "validator: the /dev/null message names the descriptor" \
  'not a readable stream'

run_case 2 "validator: an open pipe carrying no bytes fails closed" \
  "true | bash '${validator}' --diff '${work}/empty-diff.txt'"
expect_output "validator: the empty-pipe message names the producer" \
  'carried no bytes'

run_case 1 "validator: a blank PR_BODY is attributed to the env var" \
  "PR_BODY='' bash '${validator}' --diff '${work}/empty-diff.txt'"
expect_output "validator: the blank-env message names the source" \
  'description is empty .source: .PR_BODY'

run_case 0 "validator: a real pipe still reaches the parser" \
  "cat '${work}/body.md' | bash '${validator}' --diff '${work}/empty-diff.txt'"

run_case 0 "validator: --body still reaches the parser with fd 0 closed" \
  "bash '${validator}' --body '${work}/body.md' --diff '${work}/empty-diff.txt' 0<&-"

run_case 0 "validator: \$PR_BODY still reaches the parser with fd 0 on /dev/null" \
  "PR_BODY=\"\$(cat '${work}/body.md')\" bash '${validator}' --diff '${work}/empty-diff.txt' </dev/null"

echo "--- scripts/ci/ffmpeg-patches-surface-check.sh ---"

# Every surface case passes --skip-invariants (issue #3457): the #2435
# invariant tree scans run before the PR-body stage in this gate and are
# the seconds of real work that outran the 12 s hang budget under a
# parallel pre-push run. This test measures the fd-0 decision, not the
# tree scans; the scans keep their own coverage in the pre-push
# affected-tests hook.
#
# An absent body is legal for this gate: it makes the
# `no ffmpeg-patches update needed:` opt-out unclaimable, and the gate then
# decides on the diff alone. With HEAD..HEAD no public header moved, so the
# correct answer to all three shapes is a clean PASS — arrived at, not hung.

run_case 0 "ffmpeg-surface: closed fd 0 is decided, not a hang" \
  "${same_rev} bash '${ffmpeg_surface}' --skip-invariants 0<&-"
expect_output "ffmpeg-surface: the closed-fd case names what fd 0 was" \
  'PR body from empty .closed stdin.'

run_case 0 "ffmpeg-surface: </dev/null is decided, not read as a body" \
  "${same_rev} bash '${ffmpeg_surface}' --skip-invariants </dev/null"
expect_output "ffmpeg-surface: the /dev/null case names what fd 0 was" \
  'PR body from empty .unreadable stdin.'

run_case 0 "ffmpeg-surface: a real pipe still reaches the opt-out parser" \
  "printf 'no ffmpeg-patches update needed: regression test\n' | ${same_rev} bash '${ffmpeg_surface}' --skip-invariants"
expect_output "ffmpeg-surface: the piped opt-out was honoured" \
  'opt-out claimed in PR body'

# A failing `git diff` of the public headers must end the check, not be read as
# "no header changed" (the old `|| true` did exactly that and passed the gate).
# The shim reaches the real git for everything and fails only the header
# pathspec, and only while PLANT_GIT_FAILURE is set.
mkdir -p "${work}/shim"
cat >"${work}/shim/git" <<'SHIM'
#!/usr/bin/env bash
if [ -n "${PLANT_GIT_FAILURE:-}" ]; then
  case "$*" in
    *'core/include/libvmaf/*.h'*)
      echo "git: planted failure" >&2
      exit 128
      ;;
  esac
fi
exec "${REAL_GIT}" "$@"
SHIM
chmod +x "${work}/shim/git"
shim_env="REAL_GIT='$(command -v git)' PATH='${work}/shim':\"\$PATH\" ${same_rev}"

run_case 2 "ffmpeg-surface: a failing git diff of the headers fails closed" \
  "PLANT_GIT_FAILURE=1 ${shim_env} bash '${ffmpeg_surface}' --skip-invariants </dev/null"
expect_output "ffmpeg-surface: the failure names the status" \
  'command failed with status 128'

run_case 0 "ffmpeg-surface: the same shim without the planted failure passes" \
  "${shim_env} bash '${ffmpeg_surface}' --skip-invariants </dev/null"

echo "--- scripts/ci/state-md-touch-check.sh ---"

# PR_TITLE carries a Conventional-Commit `fix:` prefix, so the ADR-0165
# trigger predicate fires on every case below. HEAD..HEAD touches no
# docs/state.md, so without the `no state delta:` opt-out the gate must
# FAIL — the point being that it reaches a verdict instead of hanging.
state_env="PR_TITLE='fix: a bug-shaped title' ${same_rev}"

run_case 1 "state-md: closed fd 0 fails closed, it does not hang" \
  "${state_env} bash '${state_touch}' 0<&-"
expect_output "state-md: the closed-fd case names what fd 0 was" \
  'PR body from empty .closed stdin.'
expect_output "state-md: the closed-fd case still reaches the trigger predicate" \
  'docs/state.md drift'

run_case 1 "state-md: </dev/null fails closed" \
  "${state_env} bash '${state_touch}' </dev/null"
expect_output "state-md: the /dev/null case names what fd 0 was" \
  'PR body from empty .unreadable stdin.'

run_case 0 "state-md: a real pipe still reaches the opt-out parser" \
  "printf 'no state delta: regression test\n' | ${state_env} bash '${state_touch}'"
expect_output "state-md: the piped opt-out was honoured" \
  'PASS — opt-out'

echo "--- scripts/ci/pr-body-input.sh (descriptor hygiene) ---"

# pr_body_classify_stdin duplicates fd 0 so no later command substitution can
# steal it. That duplicate is the caller's to release: pr_body_read_stdin runs
# inside `$( )`, so closing it there would close the subshell's copy and leave
# the caller's open, inherited by every git / python3 / mktemp the gate spawns
# afterwards. The probe reads a body the supported way, then asks a child
# whether the descriptor is still there.
#
# `declare -F` rather than calling pr_body_close_stdin unconditionally: run
# against a helper that lacks the function, the probe must still report the
# leak it is looking for instead of dying at "command not found".
cat >"${work}/fd-release-probe.sh" <<'PROBE'
#!/usr/bin/env bash
# Argument 1 is the pr-body-input.sh to probe; stdin carries the body.
set -euo pipefail
. "${1:?helper path required}"

pr_body_classify_stdin
if [ "${PR_BODY_STDIN_KIND}" != "stream" ]; then
  echo "probe: expected a stream, got '${PR_BODY_STDIN_KIND}'" >&2
  exit 2
fi
probe_fd="${PR_BODY_STDIN_FD}"

body="$(pr_body_read_stdin)"
if [ "${body}" != "probe body" ]; then
  echo "probe: the body did not survive the read: [${body}]" >&2
  exit 2
fi

# Guarded, not called outright: run against a helper that has no
# pr_body_close_stdin the probe must still report the leak it is looking for,
# rather than dying at "command not found" for an unrelated reason.
if declare -F pr_body_close_stdin >/dev/null 2>&1; then
  pr_body_close_stdin
fi

if bash -c "[ -e /proc/self/fd/${probe_fd} ]"; then
  echo "probe: LEAKED descriptor ${probe_fd} — a child inherited the duplicate of fd 0" >&2
  exit 1
fi
echo "probe: descriptor ${probe_fd} released before any child was spawned"
PROBE

if [ -e /proc/self/fd ]; then
  run_case 0 "helper: the duplicate of fd 0 is released, not inherited by children" \
    "printf 'probe body' | bash '${work}/fd-release-probe.sh' '${helper}'"
  expect_output "helper: the probe confirms the descriptor was released" \
    'descriptor [0-9]+ released'
else
  echo "SKIP: helper descriptor probe needs /proc/self/fd"
fi

echo "--- planted hangs (issue #3457): the HUNG verdict stays armed ---"

# A gate that blocks must be reported as HUNG, never as a slow pass. Three
# planted shapes, all through run_case's own "hung" expectation so the
# verdict and the real cases share one code path:
#
#   - portable: the gate's stdin is a pipe whose writer never writes, so
#     `cat` blocks on read in every environment;
#   - decision-line: the gate prints its PR-body decision and then blocks,
#     which is exactly the shape a slow-pass shortcut would miscount — the
#     first kill carries the decision line, so this case exercises the 60 s
#     re-run, and its second timeout must land on HUNG;
#   - the exact pre-fix repro (`PR_BODY="$(cat)"` on a closed fd 0): on
#     Linux the command-substitution pipe takes the freed descriptor 0 and
#     `cat` reads the pipe it writes to. MSYS reports EBADF on a closed fd
#     instead of hanging, so the shape is probed first and skipped where it
#     cannot exist.

run_case hung "planted: a gate blocked on a silent stdin is reported as HUNG" \
  "sleep 30 2>/dev/null | bash -c 'PR_BODY=\"\$(cat 2>/dev/null)\"'"

run_case hung "planted: a gate that prints the decision line and then blocks on stdin is reported as HUNG" \
  "echo 'planted-gate: PR body from empty (closed stdin), diff from env'; sleep 30 2>/dev/null | cat"

# shellcheck disable=SC2016  # the planted snippet must reach the probe unexpanded
probe_code=0
timeout 3 bash -c 'PR_BODY="$(cat)"' 0<&- >/dev/null 2>&1 || probe_code=$?
if [ "${probe_code}" -eq 124 ]; then
  run_case hung "planted: a gate stuck reading a closed fd 0 is reported as HUNG" \
    "bash -c 'PR_BODY=\"\$(cat)\"' 0<&-"
else
  echo "SKIP: planted closed-fd case: this bash reports exit=${probe_code} on a closed fd 0 instead of hanging (probe), so the shape cannot exist here"
fi

echo ""
echo "test-pr-body-input-selection: ${pass_count} passed, ${fail_count} failed"
[ "${fail_count}" -eq 0 ]
