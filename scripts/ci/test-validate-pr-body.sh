#!/usr/bin/env bash
# scripts/ci/test-validate-pr-body.sh — exercises the local PR-body
# deliverables validator against the same parser shapes that have
# tripped real PRs (#461, #438, #470, #473, #486, #511, #468, #526).
#
# Each case feeds a synthetic PR body + diff into validate-pr-body.sh
# and asserts the expected exit code. On failure prints the captured
# stderr/stdout for debugging.

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
validator="${repo_root}/scripts/ci/validate-pr-body.sh"

if [ ! -x "${validator}" ]; then
  echo "test-validate-pr-body: validator missing at ${validator}" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# Common scaffolding the PR template provides; all six deliverables in
# their canonical, parser-correct shape.
canonical_body() {
  cat <<'EOF'
## Summary

Test PR body for the validator harness.

## Deep-dive deliverables

- [x] **Research digest** — `docs/research/0001-foo.md`.
- [x] **Decision matrix** — captured in the ADR.
- [x] **`AGENTS.md` invariant note** — added.
- [x] **Reproducer / smoke-test command** — pasted below.
- [x] **CHANGELOG fragment** — `changelog.d/added/foo.md`.
- [x] **Rebase note** — `docs/rebase-notes.md`.

### Reproducer

```bash
make test
```
EOF
}

pass_count=0
fail_count=0

# expect_exit <name> <expected_exit> <body_file> <diff_file>
expect_exit() {
  local name="$1" expected="$2" body="$3" diff="$4"
  local out exit_code=0
  out="$("${validator}" --body "${body}" --diff "${diff}" 2>&1)" || exit_code=$?
  if [ "${exit_code}" -eq "${expected}" ]; then
    echo "PASS: ${name} (exit=${exit_code})"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} (got exit=${exit_code}, expected ${expected})"
    echo "----- captured output -----"
    printf '%s\n' "${out}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi
}

# ---------- Case 1: ticked + all referenced files in diff -> pass ----------
canonical_body >"${work}/case1.body"
{
  echo "docs/research/0001-foo.md"
  echo "docs/rebase-notes.md"
  echo "changelog.d/added/foo.md"
  echo "src/foo.c"
} >"${work}/case1.diff"
expect_exit "ticked + files present in diff" 0 "${work}/case1.body" "${work}/case1.diff"

# ---------- Case 2: ticked Research digest BUT no docs/research/* in diff -> fail ----------
canonical_body >"${work}/case2.body"
{
  echo "docs/rebase-notes.md"
  echo "changelog.d/added/foo.md"
  echo "src/foo.c"
} >"${work}/case2.diff"
expect_exit "ticked Research digest, no file in diff" 1 "${work}/case2.body" "${work}/case2.diff"

# ---------- Case 3: numbered-list shape (NOT `- [x]`) -> fail ----------
cat >"${work}/case3.body" <<'EOF'
## Deep-dive deliverables

1. **Research digest** — `docs/research/0001-foo.md`.
2. **Decision matrix** — captured in the ADR.
3. **`AGENTS.md` invariant note** — added.
4. **Reproducer / smoke-test command** — pasted below.
5. **CHANGELOG fragment** — `changelog.d/added/foo.md`.
6. **Rebase note** — `docs/rebase-notes.md`.
EOF
{
  echo "docs/research/0001-foo.md"
  echo "docs/rebase-notes.md"
  echo "changelog.d/added/foo.md"
} >"${work}/case3.diff"
expect_exit "numbered-list shape (no - [x])" 1 "${work}/case3.body" "${work}/case3.diff"

# ---------- Case 4: opt-out without sentinel -> fail ----------
# All six unticked, no `no <thing> needed: <reason>` lines anywhere.
cat >"${work}/case4.body" <<'EOF'
## Deep-dive deliverables

- [ ] **Research digest**
- [ ] **Decision matrix**
- [ ] **`AGENTS.md` invariant note**
- [ ] **Reproducer / smoke-test command**
- [ ] **CHANGELOG fragment**
- [ ] **Rebase note**
EOF
: >"${work}/case4.diff"
expect_exit "unticked, no opt-out sentinel" 1 "${work}/case4.body" "${work}/case4.diff"

# ---------- Case 5: sentinel present but checkbox still ticked -> pass ----------
# The current parser is permissive here: a ticked box is sufficient on
# its own. Document that behaviour so a future tightening of the
# parser breaks this test loudly.
cat >"${work}/case5.body" <<'EOF'
## Deep-dive deliverables

- [x] **Research digest** — `docs/research/0001-foo.md`.
- [x] **Decision matrix** — in ADR.
- [x] **`AGENTS.md` invariant note** — added.
- [x] **Reproducer / smoke-test command** — below.
- [x] **CHANGELOG fragment** — `changelog.d/added/foo.md`.
- [x] **Rebase note** — `docs/rebase-notes.md`.

no rebase impact: still ticked, parser accepts the tick.
EOF
{
  echo "docs/research/0001-foo.md"
  echo "docs/rebase-notes.md"
  echo "changelog.d/added/foo.md"
} >"${work}/case5.diff"
expect_exit "sentinel + ticked (parser-permissive)" 0 "${work}/case5.body" "${work}/case5.diff"

# ---------- Case 6: full opt-out via sentinels, all unticked -> pass ----------
cat >"${work}/case6.body" <<'EOF'
## Deep-dive deliverables

- [ ] **Research digest** — no digest needed: trivial.
- [ ] **Decision matrix** — no alternatives: only-one-way fix.
- [ ] **`AGENTS.md` invariant note** — no rebase-sensitive invariants.
- [ ] **Reproducer / smoke-test command** — no reproducer needed: docs-only.
- [ ] **CHANGELOG fragment** — no changelog needed: internal refactor.
- [ ] **Rebase note** — no rebase impact: docs-only.
EOF
: >"${work}/case6.diff"
expect_exit "all six opted-out via sentinels" 0 "${work}/case6.body" "${work}/case6.diff"

# ---------- Case 7: ticked CHANGELOG without changelog.d/* in diff -> fail ----------
canonical_body >"${work}/case7.body"
{
  echo "docs/research/0001-foo.md"
  echo "docs/rebase-notes.md"
  # no changelog.d/* file
  echo "src/foo.c"
} >"${work}/case7.diff"
expect_exit "ticked CHANGELOG, no fragment in diff" 1 "${work}/case7.body" "${work}/case7.diff"

# ---------- Case 8: ticked Rebase note without docs/rebase-notes.md in diff -> fail ----------
canonical_body >"${work}/case8.body"
{
  echo "docs/research/0001-foo.md"
  echo "changelog.d/added/foo.md"
} >"${work}/case8.diff"
expect_exit "ticked Rebase note, no rebase-notes.md in diff" 1 "${work}/case8.body" "${work}/case8.diff"

# ---------- Summary ----------
echo ""
echo "test-validate-pr-body: ${pass_count} passed, ${fail_count} failed"
[ "${fail_count}" -eq 0 ]
