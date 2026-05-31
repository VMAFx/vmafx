<!-- markdownlint-disable MD060 -->
# GitHub Actions custom-action + reusable-workflow audit — 2026-05-31

**Status**: Audit complete. No gaps found; two abstraction candidates
documented for future opportunistic pickup. See
[ADR-0951](../adr/0951-github-actions-custom-audit.md).

## Scope

The user asked for an audit of the `.github/` tree on the fork covering:

1. Existence of custom composite/JS actions under `.github/actions/`.
2. Reusable workflows declaring `on: workflow_call:` under
   `.github/workflows/`.
3. For any found in (1) or (2):
   - SHA-pinned external `uses:` references.
   - Declared input/output schema.
   - Description present.
   - Error-handling for missing inputs.
4. If gaps: fix or document them.
5. If duplication suggests new abstractions: propose 1–2 reusable
   workflows or composite actions that would dedupe across many
   workflow files.

## Findings

### (1) Custom actions: none exist

```text
$ find .github/actions -name 'action.yml' -o -name 'action.yaml' 2>/dev/null
(no output)

$ ls .github/
AGENTS.md CODEOWNERS PULL_REQUEST_TEMPLATE.md
codeql-config.yml dependabot.yml.disabled
FUNDING.yml ISSUE_TEMPLATE/ workflows/
```

The fork has never carried a custom composite or JavaScript action.
All 24 workflow files inline their steps directly or invoke published
external actions (SHA-pinned per `.github/AGENTS.md`).

### (2) Reusable workflows: none exist

```text
$ grep -l 'workflow_call:' .github/workflows/*.yml
(no output)
```

No workflow in the tree declares `on: workflow_call:`. Two
workflow-dispatch-only files exist (`upstream-watcher.yml`,
`nightly-bisect.yml`) but they are not reusable from other workflows.

### (3) Checklist N/A

Since (1) and (2) both returned empty, the four checklist items
(SHA-pinning of external refs inside a custom action, declared
input/output schema, description, error-handling for missing inputs)
do not apply.

The fork-wide SHA-pin policy for external `uses:` directives is
already enforced and was independently re-verified during this audit:

```text
$ grep -hnE '^\s*(- )?uses:\s+[^@]+@[^ #]+\s*$' .github/workflows/*.yml \
    | grep -vE '@[a-f0-9]{40}' \
    | grep -v 'slsa-framework/slsa-github-generator/.github/workflows/'
(no output — clean)
```

The single permitted exception (`slsa-framework/slsa-github-generator`
keeping its `vX.Y.Z` tag form) is documented inline in
`supply-chain.yml` and in `.github/AGENTS.md` lines 109–116.

### (4) Gaps: none requiring fix

Nothing to fix or document beyond this digest + the ADR.

### (5) Abstraction candidates (deferred)

The audit surfaced two recurring duplication patterns across the 24
workflows. Both are documented here as candidates for the next
contributor who edits the affected files; neither is migrated in this
PR (see ADR-0951 `## Decision`).

#### Candidate A: `.github/actions/setup-build-deps` (composite action)

The most-duplicated boilerplate is the `apt-get install` line for the
CPU build toolchain. Eight call sites repeat substantially the same
list:

| Workflow | Line |
|---|---|
| `lint-and-format.yml` | L72, L414 |
| `security-scans.yml` | L118 |
| `supply-chain.yml` | L41 |
| `tests-and-quality-gates.yml` | L61, L135, L184, L303, L406, L477 |

The shared core is `meson ninja-build nasm pkg-config`. Call-site
extras (`libavcodec-dev`, `clang-tidy-18`, `intel-oneapi-compiler-*`,
`libcudart12`, etc.) are heterogeneous enough that the composite would
need an `extra-packages:` input — but a single-input composite still
cuts ~8 LOC × 8 sites = ~64 LOC of duplication and centralises pin
bumps.

Proposed shape:

```yaml
# .github/actions/setup-build-deps/action.yml
name: "Set up libvmaf CPU build dependencies"
description: |
  Install the canonical apt packages required to build libvmaf on
  Ubuntu runners (meson, ninja, nasm, pkg-config) plus any
  caller-provided extras. Idempotent; safe to call multiple times.
inputs:
  extra-packages:
    description: "Whitespace-separated extra apt packages (optional)."
    required: false
    default: ""
runs:
  using: composite
  steps:
    - name: apt update + install
      shell: bash
      run: |
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
          meson ninja-build nasm pkg-config ${{ inputs.extra-packages }}
```

Risk: low (apt-get is already idempotent; the extra-packages input is
optional so call-site overrides remain easy).

#### Candidate B: reusable `meson-cpu-build.yml` workflow

A secondary pattern is the CPU-only meson build invocation, which
appears in `tests-and-quality-gates.yml` (8 occurrences),
`sanitizers.yml` (3), and `lint-and-format.yml` (3). The pattern:

```yaml
- working-directory: core
  run: |
    meson setup build -Denable_cuda=false -Denable_sycl=false \
      -Denable_float=true --buildtype=release
    meson compile -C build
```

A reusable workflow would need to surface 4–5 inputs (`buildtype`,
`avx512`, `float`, `extra-flags`) to cover the per-site variation,
which dilutes the abstraction. The audit recommends **not** building
this until the variation collapses, OR until ADR-0951 Candidate A
lands and the residual duplication still hurts.

## Reproducer

```bash
# Confirm no custom actions exist
find .github/actions -name 'action.yml' -o -name 'action.yaml' 2>/dev/null

# Confirm no reusable workflows exist
grep -l 'workflow_call:' .github/workflows/*.yml

# Confirm SHA-pin invariant holds
grep -hnE '^\s*(- )?uses:\s+[^@]+@[^ #]+\s*$' .github/workflows/*.yml \
  | grep -vE '@[a-f0-9]{40}' \
  | grep -v 'slsa-framework/slsa-github-generator/.github/workflows/'
```

All three commands return empty on `origin/master` as of
`e81b13352`.

## Next actions

- None forced by this audit.
- Optional follow-up: when the next PR substantially edits one of the
  8 apt-install call sites listed above, extract Candidate A in the
  same PR.
