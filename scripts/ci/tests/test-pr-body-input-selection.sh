#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/tests/test-pr-body-input-selection.sh — the ADR-0108 PR-body
# gates must decide what fd 0 is before reading it.
#
# Both entry points once chose their input with `[ ! -t 0 ]`, which answers
# "is fd 0 something other than a terminal" — not "did anybody pipe me a PR
# body". Three shapes fall in the gap, and this file pins all three for
# `scripts/ci/deliverables-check.sh` and `scripts/ci/validate-pr-body.sh`:
#
#   closed fd 0   `bash gate.sh 0<&-`. The old code ran `PR_BODY="$(cat)"`,
#                 the command substitution's pipe took the freed descriptor
#                 0, `cat` read the pipe it was writing to, and the gate
#                 HUNG — measured at the 12 s timeout below, unbounded in
#                 reality. Every case here is wrapped in `timeout` and a
#                 124 is reported as a hang, not merely as a wrong code.
#   /dev/null     `bash gate.sh </dev/null`, the shape a CI step, a git
#                 hook and `nohup` all produce. Not a terminal, carries no
#                 body, and must be named as such.
#   empty pipe    an open pipe that carried no bytes: the producer sent
#                 nothing, and the gate must fail closed rather than parse
#                 an empty string into six missing deliverables.
#
# The positive controls in the same file are the point of the exercise: a
# real pipe, a real $PR_BODY and a real --body file must still be read.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
deliverables="${repo_root}/scripts/ci/deliverables-check.sh"
validator="${repo_root}/scripts/ci/validate-pr-body.sh"

for script in "${deliverables}" "${validator}"; do
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

# run_case <expected-exit> <name> <shell-snippet>
#
# The snippet runs under `timeout 12 bash -c`, from the repo root, with
# PR_BODY unset unless the snippet sets it. Exit 124 is reported as a hang
# because that is the regression this file exists to catch.
run_case() {
  local expected="$1" name="$2" snippet="$3"
  local out="" code=0
  out="$(cd "${repo_root}" && timeout 12 env -u PR_BODY bash -c "${snippet}" 2>&1)" || code=$?
  printf '%s' "${out}" >"${work}/last-output.txt"
  if [ "${code}" -eq 124 ]; then
    fail "${name} — HUNG (killed at 12 s); the gate must decide, not block"
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

echo ""
echo "test-pr-body-input-selection: ${pass_count} passed, ${fail_count} failed"
[ "${fail_count}" -eq 0 ]
