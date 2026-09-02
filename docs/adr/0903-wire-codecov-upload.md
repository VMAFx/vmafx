<!-- markdownlint-disable MD060 -->
# ADR-0903: Wire Codecov upload into the existing Coverage Gate jobs

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: ci, coverage, codecov, observability, fork-local

## Context

PR #383 (README badge audit) explicitly documented a gap: the fork ships
two in-tree coverage gates (`Coverage Gate (Ramping to 70% / 85%
Critical)` for the CPU build, `Coverage Gate — GPU Backends (Advisory)`
for the self-hosted CUDA + SYCL + AVX-512 build, both under
`.github/workflows/tests-and-quality-gates.yml`), but no upload step to
an external dashboard. The PR's note read: paraphrased — "Codecov badge
intentionally NOT added: no Codecov upload step exists in any workflow."

The in-tree gcovr-based gates (ADR-0114 per-file overrides, ADR-0117
warning-noise suppression, ADR-0637 floor history) remain the
authoritative threshold gate — they block merges when overall line
coverage drops below 37 % (CPU) / 70 % (GPU) or critical-file coverage
below 85 %. They do not, however, render a coverage trend over time,
attach diff coverage to PR comments, or expose per-file deltas in a
browsable UI. Those are external-tool concerns; Codecov fills exactly
that slot.

The fork is a public OSS repository (`VMAFx/vmafx`); Codecov supports
fork-aware OIDC since 2024 (`codecov-action` v4+), so the upload runs
without a `CODECOV_TOKEN` secret — the action mints a short-lived OIDC
token from GitHub Actions, Codecov validates the `repo:VMAFx/vmafx`
claim, and no long-lived credential lives in repo secrets. This avoids
the historical Codecov-token-leak failure mode that motivated several
upstream supply-chain incidents.

## Decision

Add a `codecov/codecov-action` step to both the `coverage` (CPU) and
`coverage-gpu` (self-hosted) jobs in
`.github/workflows/tests-and-quality-gates.yml`, immediately after the
existing `Upload coverage artifact` step. The step:

- consumes the Cobertura XML the existing gcovr step already produces
  (`core/build-coverage/coverage.xml`,
  `core/build-coverage-gpu/coverage.xml`) — no new build target, no new
  artifact, no behavioural change to the existing gate logic;
- runs under `id-token: write` job-level permission (added to both
  jobs; the workflow-level default of `contents: read` is otherwise
  preserved) and passes `use_oidc: true` so no `CODECOV_TOKEN` secret
  is required;
- sets `fail_ci_if_error: false` — the gcovr threshold gate
  (`scripts/ci/coverage-check.sh`) already blocks merges on coverage
  regression; double-gating on Codecov upload availability would create
  spurious CI failures whenever the Codecov service has an outage. This
  is consistent with the PR #338 / ADR-0114 stance that the gcovr gate
  is the source of truth;
- runs under `if: always()` so a failed test step still attempts the
  upload (matches the artifact-upload step's behaviour); and
- tags uploads with `flags: cpu` vs `flags: gpu` so Codecov's
  flag-aware UI separates the two coverage flavours, matching the
  two-job split.

The action is SHA-pinned to `cddd853df119a48c5be31a973f8cd97e12e35e16`
(the commit backing the `v6.0.1` tag, released 2026-05-18) per the
fork's `helpers:pinGitHubActionDigests` Renovate policy (ADR-0263
supply chain, ADR-0626 imposter-commit hardening, ADR-0363 Renovate as
the maintenance bot for action SHAs).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Wire `codecov/codecov-action` (chosen)** | Industry-standard; fork-aware OIDC eliminates token management; existing gcovr step already emits Cobertura XML; flag-aware UI separates CPU vs GPU views | Adds a 3rd-party dependency on the Codecov service; one more action SHA for Renovate to track | Lowest-friction path to closing the PR #383 gap; the gcovr gate remains the authoritative threshold check, so Codecov outages cannot block merges |
| Coveralls (`coverallsapp/github-action`) | Similar feature set; also OIDC-capable | Smaller adoption in C / C++ ecosystem; lcov-tracefile is its native input (we would have to add `--coveralls` to the gcovr invocation or a converter); no clear advantage for our use case | Codecov is the de-facto standard for public OSS C projects and integrates more cleanly with the existing Cobertura XML |
| Self-hosted dashboard (e.g. Grafana + parsing `coverage.json`) | Full control; no third-party data sharing | Operational burden (host, auth, persistence, alerting); replicates infra the team is not asking to operate; no PR-comment integration | Disproportionate cost for what is fundamentally a "render gcovr output prettier" need |
| Custom PR-comment job parsing `coverage.json` | Self-hosted; no third party | Reinvents Codecov's diff-coverage / per-file UI; long-term maintenance debt | Yak shave; the off-the-shelf tool already exists |
| Codecov **with** `fail_ci_if_error: true` | Strict on upload failure | A Codecov outage would block merges even though the gcovr gate has already approved the change | Violates correctness-first / no-double-gating principle |
| Codecov **with** static `CODECOV_TOKEN` secret | Works for private repos | Long-lived credential; rotation overhead; supply-chain blast radius if leaked | We are public; OIDC is strictly better |
| Do nothing — leave the gap | Zero work | Trend / diff-coverage visibility never materialises; PR #383's note becomes a permanent TODO | Gap was explicitly flagged; default per task brief is "wire it" |

## Consequences

- **Positive**: PR-comment diff coverage and a public trend dashboard
  for any contributor to glance at, without standing up our own
  infrastructure.
- **Positive**: A Codecov badge can be added to `README.md` in a
  follow-up (the badge surface PR #383 deferred for exactly this
  reason) and will reflect real data rather than the "unknown" state
  that motivated the deferral.
- **Positive**: OIDC means zero new secrets in the repo; nothing to
  rotate, nothing to leak.
- **Neutral**: Adds two new action steps but no new build artefacts
  and no change to the threshold logic — the gcovr `Coverage Gate` job
  remains the only blocking gate.
- **Neutral**: Adds `codecov/codecov-action` to the Renovate
  SHA-pinning track. Renovate already manages every other action in
  the workflow (ADR-0363); the marginal cost is one more weekly PR.
- **Negative**: Introduces a runtime dependency on a third-party
  service for the *informational* coverage view. Mitigated by
  `fail_ci_if_error: false`: outages do not break CI.

## References

- PR #383 — README badge audit; documented the gap this ADR closes
  (paraphrased: "Codecov badge intentionally NOT added: no Codecov
  upload step exists in any workflow").
- [ADR-0114](0114-coverage-gate-per-file-overrides.md) — the
  authoritative gcovr-based threshold gate this Codecov upload
  complements (not replaces).
- [ADR-0117](0117-coverage-gate-warning-noise-suppression.md) —
  suspicious/negative-hit filter on the gcovr stderr.
- [ADR-0637](0637-coverage-gate-floor-37pct-after-may-19-merge-burst.md) —
  current floor history.
- PR #338 — fixed the gcovr gate after a `vmaf_ort_output_name_at`
  uncovered-line breach; cited as the reason `fail_ci_if_error:
  false` is correct ("don't double-gate; the gcovr step already
  blocks").
- [ADR-0263](0263-ossf-scorecard-policy.md) — supply-chain policy
  requiring third-party actions to be SHA-pinned.
- [ADR-0363](0363-renovate-replaces-dependabot.md) — Renovate is the
  bot that maintains the codecov-action SHA over time.
- [ADR-0626](0626-macos-ci-tmate-debug-on-failure.md) — imposter-commit
  hardening; cites the same SHA-pinning requirement.
- Codecov OIDC documentation:
  <https://docs.codecov.com/docs/codecov-tokens#oidc-token-authentication>
- Source: `req` — paraphrased from session brief: "PR #383 audit found
  Codecov badge intentionally NOT added because no Codecov upload step
  exists in any workflow. Fix that — wire it up."
