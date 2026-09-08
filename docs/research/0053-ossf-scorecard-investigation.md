# Research-0053: OSSF Scorecard investigation and remediation plan

- **Status**: Historical May investigation, corrected 2026-09-08
- **Workstream**: ADR-0263, superseded by ADR-1247
- **Last updated**: 2026-09-08

## 2026-09-08 correction and measured gate design

The May observations below are retained as history. They are not current
security evidence, and several causal claims were wrong:

- The public result at `2026-09-08T16:15:55Z` identifies master
  `78c9d2bfc580880919d5168c18ad19441f9db96a`, Scorecard `v5.5.0` at
  `c395761df6afe1a69e476bc60a013a94bcbc153f`, aggregate **8.8**. CII and
  Code-Review are **0**, License **9**, and twelve checks **10**. Pinning,
  Branch-Protection and Signed-Releases are **-1**; the aggregate excludes
  them. It is not an acceptance report for the RC1 integration branch.
- Exact BuildKit 0.26.3 and the Scorecard raw callback reproduce an actual
  unterminated heredoc in `docker/dev/fedora-40.Dockerfile`. Its optional
  SYCL RUN has a literal backslash-n terminator. The root Dockerfile parses.
  The earlier root `while`/redirect hypothesis is disproved; that root block
  needs no speculative rewrite. The parser component records its own controls.
- Live classic protection lacked required PR reviews, admin enforcement and
  strict status checks; no ruleset was present. Those are real policy gaps,
  not merely token visibility. The pinned action documents rulesets readable
  with its default ephemeral token; a stored PAT is not the only remedy.
- The repository license is **BSD-2-Clause-Patent**, as its LICENSE and README
  state. The May BSD-3-Clause-Plus-Patent/BSD-3-Clause-Clear identification and
  claimed explanation of the one-point license deduction were unsupported.
- No fork release exists in the retained live query. First-release signing
  must be verified on actual artifacts; neither a future score of 10 nor
  historical `v3.x.y-lusoris.N` fork versioning is a current contract.
- CII registration and independent review are remediation work, not permanent
  waivers. A Gold goal is not an earned badge. See current policy
  [ADR-1247](../adr/1247-scorecard-exact-head-gates.md) and the
  [operator guide](../development/ossf-scorecard.md).

The exact pinned action's `main.go` writes `results.json` alongside SARIF
before signing/publishing. Its `options/options.go` selects local directory
mode on PRs and default remote HEAD otherwise; `internal/scorecard/scorecard.go`
does not pass an event SHA to the library. Scorecard's `pkg/scorecard/scorecard.go`
limits explicit-commit and local runs by request type. Consequently, a full
18-check master score is not available for an unmerged PR. We preserve the
restricted publisher and validate its same-run full report in another job;
PRs get a separately labelled exact-source 11-check local gate. The GitHub
client separately fetches GraphQL history and a default-HEAD archive, so a final
read-only master-ref check must still match the report/event SHA. Under the
no-force/no-deletion branch policy this rejects forward movement during the
scan; an administrator changing and restoring policy is outside that guarantee.

A real local schema/parser control with exact Scorecard 5.5.0/c395761d and the
reviewed Fedora fix returned eleven checks and aggregate **9.7**: License 9,
Pinned-Dependencies 7 and all other local checks 10. The parser now reports
actual unpinned dependencies instead of hiding them behind an error. This was
a disposable source overlay control, not hosted or final-candidate acceptance.
Gate fixtures additionally reject stale heads, missing/duplicate checks, forged
rounded scores, any scanner error and modified/untracked inputs, and execute
the actual aggregator JavaScript against absence/skip/neutral/failure controls.
The only explicit unassessed state is the exact upstream no-release reason.

Independent review reproduced a source-binding hole in the first implementation:
a tracked Dockerfile symlink could read `.git/hidden-input`, changing the actual
local pinning score from 10 to 0 while both source receipts stayed identical.
The corrected binder verifies every followed symlink component against tracked
files/directories, rejects mutable Git metadata and escaping/missing/cyclic
targets, and preserves literal targets and legitimate directory-link chains.
The original real-scanner negative evidence is retained with the fix controls.

Implementation evidence is retained under
`.workingdir2/evidence/scorecard-policy-20260908/`; its manifest distinguishes
local controls from subsequent hosted acceptance and links raw report hashes.
Official source anchors are pinned in ADR-1247 rather than following `main`.

## Historical May question

The OpenSSF Scorecard workflow on `master` has been red on every push
for an extended period, and the public dashboard at
`https://scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx` shows an
aggregate score of 6.2/10. Why is the workflow failing, which
individual checks are dragging the score down, and which of those are
addressable inside the fork without external services or paid
tooling?

## Sources

- Scorecard run logs (project-internal): `gh run view 25281489510
  --repo VMAFx/vmafx --log` (the SARIF JSON containing the per-check
  scores is embedded verbatim in the `Run Scorecard` step output).
- Scorecard checks reference:
  <https://github.com/ossf/scorecard/blob/main/docs/checks.md>
- Scorecard action workflow restrictions (imposter-commit detection):
  <https://github.com/ossf/scorecard-action#workflow-restrictions>
- `github/codeql-action` v4 tag resolution (queried 2026-05-03):
  GitHub API `git/refs/tags/v4` →
  `e46ed2cbd01164d986452f91f178727624ae40d7`.
- Historical license identification (incorrect; see correction above):
  the previously cited BSD-3-Clause-Clear metadata and the
  Scorecard `License` check normaliser source.

## Findings

### Aggregate score on the latest scan

- Date: 2026-05-03 14:14 UTC, scorecard `v5.3.0`, head commit
  `981659a3e2b777953221688dd686c3857e0e7b8a`.
- Aggregate: **6.2 / 10**.
- Workflow conclusion: **failure** (the Scorecard step itself failed,
  not the Upload-SARIF step). Root cause is separate from the score
  — see "Run-level failure" below.

### Per-check breakdown (verbatim from the SARIF results)

| Check | Score | Reason | Severity | Addressable here? |
| --- | ---: | --- | --- | --- |
| Maintained | 0 | "project was created within the last 90 days" | low | **No — auto-resolves** as the fork ages past 90 days from first push. |
| Security-Policy | 10 | `SECURITY.md` present and well-formed | — | already passing |
| Code-Review | 0 | "Found 0/30 approved changesets — score normalized to 0" | high | **No — solo-maintainer artefact**. The fork is squash-merged by the same human who opens the PR; GitHub's `reviewed=approved` event is never recorded because there's no second reviewer. Documented blocker. |
| Dangerous-Workflow | 10 | no dangerous patterns | — | already passing |
| Dependency-Update-Tool | 10 | Dependabot detected | — | already passing |
| Pinned-Dependencies | -1 | "internal error: invalid Dockerfile: unterminated heredoc" | — | **Tool bug** — Scorecard's go-dockerfile parser misreads our `Dockerfile` line 101–107 (a multi-line `RUN set -e; while … done < series.txt`). No real heredoc exists. See "Pinned-Dependencies internal error" below for the workaround plan. |
| Token-Permissions | 10 | least-privilege; one informational warn on `release-please.yml` (`contents: write`, required by the action) | — | already passing |
| Binary-Artifacts | 10 | none | — | already passing |
| Signed-Releases | -1 | "no releases found" | — | **No — no v3.x.y-lusoris.N release has been cut yet**. Scorecard re-evaluates as soon as the first signed release lands; the release-please pipeline (ADR-0118 / docs/development/release.md) is already keyless-Sigstore-signing, so the score will jump to 10 on first cut. |
| CII-Best-Practices | 0 | "no effort to earn an OpenSSF best practices badge detected" | — | **Documented gap** — the OpenSSF Best Practices Badge requires an external application at <https://www.bestpractices.dev/>. Tracked as out-of-scope per the standing rule "no tooling that requires external secrets / paid services" (this one needs a manual application but is free; revisit if the project decides to pursue silver/gold). |
| Vulnerabilities | 0 | "13 existing vulnerabilities detected" — all are PYSEC IDs in the classic-training Python stack | high | **Partial** — see "Vulnerabilities triage" below. |
| SAST | 10 | CodeQL on every commit | — | already passing |
| Fuzzing | 0 | no fuzzer integrations | medium | **Documented gap** — OSS-Fuzz onboarding for libvmaf is a multi-week external workstream. Out of scope for this PR; tracked as a follow-up. |
| Packaging | -1 | "packaging workflow not detected" | — | **Documented gap** — Scorecard looks for a published-package workflow (npm / PyPI / Maven / GitHub-release-as-package). The fork ships via container images and source tarballs from release-please, not via a registry. Revisit if/when a `.whl` or `vmaf` PyPI package is published. |
| Branch-Protection | -1 | "internal error … some github tokens can't read classic branch protection rules" | — | **Documented blocker** — the `GITHUB_TOKEN` issued by Actions cannot read classic branch protection rules; a fine-grained PAT with `Administration: read` is required (see <https://github.com/ossf/scorecard-action/blob/main/docs/authentication/fine-grained-auth-token.md>). Adding such a PAT means storing a personal token as an org secret, which the fork policy forbids ("no tooling that requires external secrets / paid services" — extended to user PATs). Master is in fact protected (19 required checks, linear history, force-push blocked) per `feedback_master_branch_protection`; the score is a tooling-visibility gap, not a real posture gap. |
| License | 9 | "license file detected" + warning "project license file does not contain an FSF or OSI license" | low | **Cosmetic** — the project's licence is `BSD-3-Clause-Plus-Patent`, which **is** OSI-approved (SPDX id `BSD-3-Clause-Clear` family). Scorecard's pattern-match fails because Netflix's text uses a non-standard preamble. A 1-pt gap not worth structural change. |
| CI-Tests | 10 | 30 / 30 merged PRs run CI | — | already passing |
| Contributors | 10 | 17 contributing organisations (upstream lineage) | — | already passing |

### Run-level failure (separate from the score)

The workflow's `Run Scorecard` step exits non-zero with:

```text
error processing signature: error sending scorecard results to webapp:
  http response 400, status: 400 Bad Request, error:
  {"code":400,"message":"workflow verification failed: workflow
   verification failed: imposter commit:
   b25d0ebf40e5b63ee81e1bd6e5d2a12b7c2aeb61 does not belong to
   github/codeql-action/upload-sarif, see
   https://github.com/ossf/scorecard-action#workflow-restrictions for
   details."}
```

The Scorecard webapp validates that every `uses:` SHA in the workflow
file belongs to its declared action. The pinned commit
`b25d0ebf40e5...` for `github/codeql-action/upload-sarif@v4` does
**not** exist in the upstream repository (verified via `gh api
/repos/github/codeql-action/commits/b25d0ebf...` → 422 "No commit
found"). It was probably the v4 head at some past moment that
upstream then rewrote (force-push to a release branch, or a tag move
between `v4` and a `v4.x.y` cut), making the SHA dangling.

Scorecard treats a dangling SHA as an "imposter commit" because, if
the action's repo got compromised and an attacker tagged a malicious
ref under the same name, a stale SHA would let the consumer continue
to point at a phantom while the upstream maintainer rotates. The fix
is to repin to a SHA that currently exists under the `v4` tag.

Resolved 2026-05-03 via GitHub API: `v4` →
`e46ed2cbd01164d986452f91f178727624ae40d7`.

### Historical pinning hypothesis (disproved above)

The check returns `-1` (internal error) with message "invalid
Dockerfile: unterminated heredoc" against the project's top-level
`Dockerfile`. There is no `<<` heredoc in any of the three
`Dockerfile*` files in the tree (`grep -nE '<<' Dockerfile*`
produces no output). The likely culprit is Scorecard's
go-dockerfile parser stumbling over the multi-line `RUN` at lines
101–107:

```dockerfile
RUN set -e; \
    while IFS= read -r line; do \
        case "$line" in ''|\#*) continue ;; esac; \
        echo "Applying ffmpeg-patches/$line"; \
        git apply "/tmp/ffmpeg-patches/$line" 2>/dev/null \
            || patch -p1 < "/tmp/ffmpeg-patches/$line"; \
    done < /tmp/ffmpeg-patches/series.txt
```

The parser appears to misclassify the `done < /tmp/...` shell
redirect as an unterminated heredoc opener. This is a known class
of false positive in Scorecard's Dockerfile lexer (similar reports
exist upstream at `ossf/scorecard` against complex `RUN` blocks).

Workarounds, ranked from least to most invasive:

1. **Leave it** — the check returns -1 ("inconclusive") rather than
   0, which doesn't drag the aggregate down (Scorecard averages
   only checks that produced a definite score). Pinned-Dependencies
   is currently *invisible* in the 6.2 aggregate, not pulling it
   down.
2. **Refactor the multi-line `RUN`** into a small shell script
   under `docker/apply-ffmpeg-patches.sh` and `RUN bash
   /tmp/.../apply.sh`, which sidesteps the Dockerfile parser
   entirely. Would be cosmetically nice but adds a moving piece to
   the build context for what is currently a parser bug, not a real
   security finding.
3. **Upstream the bug** to ossf/scorecard. Best long-term path,
   zero effort here.

This PR picks (1) for now and notes (3) as a follow-up.

### Vulnerabilities triage

Scorecard surfaces 13 OSV.dev hits, all PYSEC / GHSA in the Python
dependency tree:

- `PYSEC-2017-1`, `PYSEC-2018-33`, `PYSEC-2018-34`, `PYSEC-2019-108`,
  `PYSEC-2020-73`, `PYSEC-2020-107`, `PYSEC-2020-108`,
  `PYSEC-2021-856`, `PYSEC-2021-857`, `PYSEC-2023-102`,
  `PYSEC-2023-114`, `PYSEC-2024-110`, `GHSA-fpfv-jqm9-f5jm`.

These map to ancient versions of `numpy`, `scipy`, `Pillow`, and
related scientific-Python deps in `python/requirements.txt`, which
declares lower bounds like `numpy>=1.18.2,<2.0.0` and
`scipy>=1.4.1`. A pip-resolver run inside Scorecard's image will
choose the lower bound when it can, exposing the vuln IDs.

Two characteristics matter:

1. **The file is upstream-mirrored**. Netflix/vmaf shipped these
   bounds; tightening them in the fork is a legitimate change but
   creates rebase conflict surface (CLAUDE.md §10 + the `no-lint-skip-on-upstream`
   tells us to fix rather than exclude, but we still need to land
   the change deliberately).
2. **Runtime impact is bounded**. The classic-training harness in
   `python/vmaf/` runs offline on operator workstations to produce
   model `.pkl` / `.json` artefacts; it isn't shipped as runtime
   surface to libvmaf consumers. The threat model is "an
   inadvertent local pin during training" rather than "remote
   exploit of a shipped dependency".

The right move is a **separate, focused PR** that bumps the
lower bounds in `python/requirements.txt` to the
no-known-CVE versions (`numpy>=1.26`, `scipy>=1.13`, `Pillow>=10.4`,
etc.) plus a corresponding entry in `docs/rebase-notes.md`
describing the conflict resolution. Deferred from this PR.

## Alternatives explored

- **Apply for the OpenSSF Best Practices Badge** to lift `CII-Best-Practices`
  from 0 to ≥7. Out of scope per the project's
  no-external-service rule and because the badge is a months-long
  process; revisit when/if the project pursues higher tiers.
- **Add a fine-grained PAT to unblock `Branch-Protection`**. Rejected:
  user PATs are external secrets and the fork's secret-management
  policy disallows them. The score is cosmetic since master is in
  fact protected.
- **Onboard to OSS-Fuzz** to lift `Fuzzing` from 0. Out of scope;
  tracked as a follow-up workstream.
- **Pin every transitive `@vN` reference in the workflow tree
  (`security-scans.yml`, `nightly.yml`, ...) in this PR**. Rejected
  to keep this PR focused — Scorecard's `Pinned-Dependencies` is
  currently throwing -1 (internal error) anyway, so pinning the
  remaining refs has zero score impact until the parser bug
  upstream is fixed. Tracked as a separate sweep PR.

## Action plan (this PR vs follow-ups)

**Land in this PR**:

- [x] Repin `github/codeql-action/upload-sarif@<sha>` from the
  imposter `b25d0ebf...` to the current `v4` head
  `e46ed2cbd01164d986452f91f178727624ae40d7`. Unblocks the workflow
  failure → workflow turns green.
- [x] Write this digest + ADR-0263 establishing the
  fork's OSSF-Scorecard policy (target score, accepted blockers,
  remediation cadence).
- [x] Add the row to the docs index and the ADR README.

**Follow-up PRs** (each self-contained, one ADR / scope each):

- [ ] **Vulnerabilities sweep** — bump `python/requirements.txt`
  lower bounds to no-known-CVE versions; add `docs/rebase-notes.md`
  entry. Target: lift `Vulnerabilities` from 0 → 10. Estimated 1
  PR, < 1 day.
- [ ] **Pinned-Dependencies hygiene sweep** — replace every
  `actions/<x>@vN` with SHA pins across the workflow tree
  (currently dozens of unpinned refs in `security-scans.yml`,
  `nightly.yml`, etc.). Wait for the upstream Dockerfile-parser bug
  to be fixed first so the score actually moves; until then this
  is hygiene without observable Scorecard delta.
- [ ] **OSS-Fuzz onboarding** for libvmaf (separate research digest;
  multi-week effort).
- [ ] **Upstream the Dockerfile-parser bug** at `ossf/scorecard`
  (issue + minimal repro extracted from our top-level `Dockerfile`
  lines 101–107). Zero diff against this repo.
- [ ] **First signed release** (`v3.x.y-lusoris.0`) via release-please
  to flip `Signed-Releases` from -1 → 10.

## Open questions

- After the codeql-action repin lands, does the Scorecard webapp
  actually publish the SARIF (i.e., does the public dashboard refresh
  to a non-stale view)? The 400-error path skipped publish on every
  recent run, so the live `scorecard.dev/viewer/...` page is showing
  results from before the PR-tracked failures started. This will be
  observable once a green run completes against `master`.
- Is the Code-Review 0 score a hard ceiling for solo-maintainer
  forks, or does Scorecard count the GitHub-native "approved"
  signal even when the approver and the author are the same login?
  (Empirically, no — but worth confirming via a second-account
  approve experiment if the score becomes a hiring or compliance
  blocker.)

## Related

- ADRs: [ADR-0263](../adr/0263-ossf-scorecard-policy.md)
- Workflow: [`.github/workflows/scorecard.yml`](../../.github/workflows/scorecard.yml)
- Run log: `gh run view 25281489510 --repo VMAFx/vmafx --log`
- Public dashboard: <https://scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx>
