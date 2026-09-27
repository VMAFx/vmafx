#!/usr/bin/env bash
# Regression test for the release-please draft publication gate.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# ADR-1127: while one unpublished release draft waits at the human publication
# gate, release-please.yml must pause both release-please phases, so later
# master pushes neither move nor duplicate it. The step that finds that draft
# selects tags with an inline jq regex -- the job has no checkout, so it cannot
# call verify-release-version.sh -- and ADR-1201 release candidates
# (vX.Y.Z-rc.N) were once invisible to it. Everything below is read straight
# out of the workflow, so the test fails as soon as the workflow drifts:
#
#   1. the jq filter alone, against sample release lists;
#   2. the filter's accepted tag shape, against verify-release-version.sh's;
#   3. the whole step script, run under plain `bash` (the step sets
#      `set -euo pipefail` itself, so it never relies on the runner default) with a
#      stub `gh`, for its exists/tag outputs and its fail-closed exits (an
#      unreadable or unparseable release list must stop the job, never read as
#      "no draft");
#   4. both release-please invocations still being gated on that output.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/release-please.yml"
VERIFY="$REPO_ROOT/scripts/release/verify-release-version.sh"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0

ok() {
  printf 'PASS: %s\n' "$1"
  pass=$((pass + 1))
}

bad() {
  printf 'FAIL: %s\n' "$1" >&2
  fail=$((fail + 1))
}

# --- extraction + structural checks (4) -------------------------------------
python3 - "$WORKFLOW" "$scratch/step.sh" "$scratch/filter.jq" <<'PY'
import re
import sys
from pathlib import Path
from typing import NoReturn

RELEASE_PLEASE_PHASES = 2  # release creation, then release-PR generation

workflow, step_out, filter_out = (Path(arg) for arg in sys.argv[1:4])
lines = workflow.read_text(encoding="utf-8").splitlines()

# The single job's steps are the "      - " items; a step's block runs to the
# next one (or EOF), comments included.
starts = [index for index, line in enumerate(lines) if line.startswith("      - ")]
steps = [lines[start:end] for start, end in zip(starts, [*starts[1:], len(lines)], strict=True)]


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FAIL: {message}")


drafts = [step for step in steps if "        id: draft" in step]
if len(drafts) != 1:
    fail(f"expected exactly one step with 'id: draft', found {len(drafts)}")
step = drafts[0]
if "        run: |" not in step:
    fail("the draft step no longer has a literal 'run: |' script")

body: list[str] = []
for line in step[step.index("        run: |") + 1 :]:
    if not line.strip():
        body.append("")
        continue
    if not line.startswith("          "):
        break
    body.append(line[10:])
script = "\n".join(body).strip("\n") + "\n"
if "${{" in script:
    fail("the draft step script interpolates an expression; pass it via env:")

filters = re.findall(r"jq -r '(.*?)' \"\$releases\"", script, re.S)
if len(filters) != 1:
    fail(f"expected one jq -r '...' \"$releases\" filter, found {len(filters)}")
step_out.write_text(script, encoding="utf-8")
filter_out.write_text(filters[0], encoding="utf-8")
print("PASS: extracted the draft step script and its jq filter")

# The draft's existence must pause BOTH release-please phases (ADR-1127).
invocations = [
    step for step in steps if any("googleapis/release-please-action@" in line for line in step)
]
if len(invocations) != RELEASE_PLEASE_PHASES:
    fail(f"expected {RELEASE_PLEASE_PHASES} release-please invocations, found {len(invocations)}")
for invocation in invocations:
    if "steps.draft.outputs.exists != 'true'" not in "\n".join(invocation):
        fail(f"release-please step not gated on the draft: {invocation[0].strip()}")
print("PASS: both release-please invocations are gated on the draft")
PY
pass=$((pass + 2))

# --- stub gh: serves a fixture as `gh api --paginate --slurp` output --------
mkdir -p "$scratch/bin" "$scratch/empty-repo"
cat >"$scratch/bin/gh" <<'STUB'
#!/usr/bin/env bash
# The filter walks `.[][]`, i.e. the page-array shape `--paginate --slurp`
# produces; a caller that drops either flag would get a different shape.
args=" $* "
[[ "$1" == api && "$args" == *" --paginate "* && "$args" == *" --slurp "* &&
  "$args" == *" repos/example/repo/releases?per_page=100 "* ]] || {
  printf 'stub gh: unexpected invocation: %s\n' "$*" >&2
  exit 98
}
if [[ -n "${STUB_GH_FAIL:-}" ]]; then
  printf 'HTTP 502: Bad Gateway\n' >&2
  exit 1
fi
cat -- "$STUB_GH_FIXTURE"
STUB
chmod +x "$scratch/bin/gh"

case_no=0

# run_step FIXTURE [GH_FAILS]
run_step() {
  local fixture="$1" gh_fails="${2:-}"
  case_no=$((case_no + 1))
  step_rc=0
  step_output="$scratch/output.$case_no"
  mkdir -p "$scratch/runner.$case_no"
  : >"$step_output"
  env PATH="$scratch/bin:$PATH" STUB_GH_FIXTURE="$fixture" STUB_GH_FAIL="$gh_fails" \
    RUNNER_TEMP="$scratch/runner.$case_no" GITHUB_OUTPUT="$step_output" \
    GITHUB_REPOSITORY=example/repo GH_TOKEN=stub \
    bash "$scratch/step.sh" >/dev/null 2>&1 || step_rc=$?
}

# expect DESCRIPTION RELEASES_JSON [SELECTED_TAG...]
# The jq filter must select exactly SELECTED_TAG...; the step must then report
# exists=false for none, exists=true + tag for one, and exit 1 for several.
expect() {
  local description="$1" releases="$2"
  shift 2
  local fixture="$scratch/fixture.$((case_no + 1)).json"
  local want got
  printf '%s\n' "$releases" >"$fixture"
  want="$(printf '%s\n' "$@")"
  got="$(jq -r -f "$scratch/filter.jq" "$fixture")"
  if [[ "$got" == "$want" ]]; then
    ok "filter: $description"
  else
    bad "filter: $description (selected [${got//$'\n'/ }], want [$*])"
  fi

  run_step "$fixture"
  local exists tag
  exists="$(sed -n 's/^exists=//p' "$step_output")"
  tag="$(sed -n 's/^tag=//p' "$step_output")"
  case $# in
    0)
      if [[ "$step_rc" -eq 0 && "$exists" == false && -z "$tag" ]]; then
        ok "step: $description -> exists=false"
      else
        bad "step: $description (rc=$step_rc exists=$exists tag=$tag, want exists=false)"
      fi
      ;;
    1)
      if [[ "$step_rc" -eq 0 && "$exists" == true && "$tag" == "$1" ]]; then
        ok "step: $description -> exists=true tag=$1"
      else
        bad "step: $description (rc=$step_rc exists=$exists tag=$tag, want exists=true tag=$1)"
      fi
      ;;
    *)
      if [[ "$step_rc" -eq 1 && -z "$exists" ]]; then
        ok "step: $description -> operator cleanup (exit 1)"
      else
        bad "step: $description (rc=$step_rc exists=$exists, want exit 1 and no output)"
      fi
      ;;
  esac
}

# --- 1 + 3: sample release lists ---------------------------------------------
# Positive: an unpublished final or release-candidate draft pauses the pipeline.
expect 'v1.0.0 draft' \
  '[[{"tag_name":"v1.0.0","draft":true,"prerelease":false}]]' v1.0.0
expect 'v1.0.0-rc.1 draft' \
  '[[{"tag_name":"v1.0.0-rc.1","draft":true,"prerelease":true}]]' v1.0.0-rc.1
expect 'v1.2.3-rc.10 draft' \
  '[[{"tag_name":"v1.2.3-rc.10","draft":true,"prerelease":true}]]' v1.2.3-rc.10
expect 'v1.0.0-rc.0 draft (boundary: bare zero)' \
  '[[{"tag_name":"v1.0.0-rc.0","draft":true,"prerelease":true}]]' v1.0.0-rc.0
expect 'rc draft on the second page' \
  '[[{"tag_name":"v0.9.0","draft":false}],[{"tag_name":"v1.0.0-rc.2","draft":true}]]' \
  v1.0.0-rc.2
expect 'rc draft among published releases and foreign drafts' \
  '[[{"tag_name":"v1.0.0","draft":false},{"tag_name":"archive/foo","draft":true},
     {"tag_name":"v1.0.0-rc.1","draft":true},{"tag_name":"v1.0.0-beta","draft":true}]]' \
  v1.0.0-rc.1

# Negative: published releases and every non-release tag shape are ignored.
expect 'published final and rc releases' \
  '[[{"tag_name":"v1.0.0","draft":false},{"tag_name":"v1.0.0-rc.1","draft":false,"prerelease":true}]]'
expect 'v1.0.0-rc.01 draft (leading zero)' \
  '[[{"tag_name":"v1.0.0-rc.01","draft":true}]]'
expect 'v1.0.0-beta draft (other channel)' \
  '[[{"tag_name":"v1.0.0-beta","draft":true}]]'
expect 'archive/foo draft (not a release tag)' \
  '[[{"tag_name":"archive/foo","draft":true}]]'
expect 'v3.0.0-rc draft (no rc number)' \
  '[[{"tag_name":"v3.0.0-rc","draft":true}]]'
expect 'empty release list (one empty page)' '[[]]'
expect 'empty release list (no pages)' '[]'

# Two gate-shaped drafts: which one waits is ambiguous, so the step stops.
expect 'two rc drafts' \
  '[[{"tag_name":"v1.0.0-rc.1","draft":true},{"tag_name":"v1.0.0-rc.2","draft":true}]]' \
  v1.0.0-rc.1 v1.0.0-rc.2
expect 'an rc draft and a final draft' \
  '[[{"tag_name":"v1.0.0-rc.1","draft":true},{"tag_name":"v1.0.0","draft":true}]]' \
  v1.0.0-rc.1 v1.0.0

# The release list cannot be read or parsed: fail closed. An empty draft list
# here would read as "no draft" and release-please would run past the gate.
fails_closed() {
  local description="$1"
  if [[ "$step_rc" -ne 0 && ! -s "$step_output" ]]; then
    ok "step: $description fails closed (exit $step_rc)"
  else
    bad "step: $description (rc=$step_rc, output: $(tr '\n' ' ' <"$step_output"))"
  fi
}
: >"$scratch/unreadable.json"
run_step "$scratch/unreadable.json" fail
fails_closed 'unreadable release list'
printf '<html>502 Bad Gateway</html>\n' >"$scratch/malformed.json"
run_step "$scratch/malformed.json"
fails_closed 'malformed release list'

# --- 2: the filter accepts exactly what verify-release-version.sh accepts ----
# The verifier exits 64 when the tag shape is refused and later (1: missing
# release inputs in an empty repo root) when it is accepted.
for candidate in \
  v0.0.0 v1.0.0 v1.2.3 v10.20.30 v1.0.0-rc.0 v1.0.0-rc.1 v1.2.3-rc.10 \
  v1.0.0-rc.01 v1.0.0-rc v3.0.0-rc v1.0.0-beta v1.0.0-beta.1 v1.0.0-RC.1 \
  v1.0.0-rc.1.2 v1.0.0-rc.1+build v1.0.0+build v1.0.0- v1.0.0-rc.-1 \
  v01.0.0 v1.00.0 v1.0.01 v1.0 v1.0.0.0 V1.0.0 1.0.0 1.0.0-rc.1 \
  vmafx-v1.0.0 archive/foo archive/v1.0.0 ''; do
  filter_accepts=false
  if [[ -n "$(jq -n --arg tag "$candidate" '[[{tag_name: $tag, draft: true}]]' |
    jq -r -f "$scratch/filter.jq")" ]]; then
    filter_accepts=true
  fi
  verify_rc=0
  env VMAFX_REPO_ROOT="$scratch/empty-repo" bash "$VERIFY" "$candidate" \
    >/dev/null 2>&1 || verify_rc=$?
  verify_accepts=true
  [[ "$verify_rc" -eq 64 ]] && verify_accepts=false
  if [[ "$filter_accepts" == "$verify_accepts" ]]; then
    ok "shape parity: '$candidate' accepted=$filter_accepts"
  else
    bad "shape parity: '$candidate' draft gate=$filter_accepts verify-release-version=$verify_accepts"
  fi
done

printf '%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
