<!-- markdownlint-disable MD013 MD060 -->
# OpenSSF Best Practices passing assessment

This is a review worksheet, not a badge or a claim that VMAFx passes. OpenSSF's
[passing criteria](https://www.bestpractices.dev/en/criteria/0) comprise 67 checks
at this assessment date (2026-09-08). The badge is free, voluntary
self-certification; an authorized maintainer registers the project and supplies
truthful answers. Repository documents alone cannot replace that external
record or change Scorecard's `CII-Best-Practices` result. The registered
[VMAFx assessment, project 14549](https://www.bestpractices.dev/en/projects/14549),
is **in progress**; no passing badge is claimed. The authorized submission's
public readback at `2026-09-08T21:38:23.592Z` recorded **42%**, with 28 passing
criteria marked `Met`, 39 unknown and all 13 Basics criteria marked `Met`.
This is a dated external assessment snapshot, not an earned badge or a
percentage inferred from local checks. Recheck the project record for updates.

The source reviewed here is the RC1 candidate
`76a7c467c3524478c25d588a31b37b2d45d96081`; live `master` was `78c9d2bf`.
Candidate fixes must be published and checked at their eventual merged commit
before they become current-project assertions. GitHub Releases returned no
published VMAFx release. The first `1.0` release candidate is planned; inherited
libvmaf `3.x` identifiers are not VMAFx support or release evidence.

## How to use the worksheet

The official [achievement rules](https://www.bestpractices.dev/en/criteria_discussion#achieving-a-badge)
require all MUST/MUST NOT criteria, a result or justified exception for every
SHOULD, and consideration of every SUGGESTED item. N/A is allowed only where
specified and sometimes requires justification. “Evidence” below identifies
concrete support for a proposed answer; it does not automatically submit `Met`.
“Prepared” means a policy, future release step or bounded implementation exists
but publication/execution evidence is still needed. “Unknown” needs further
records or maintainer knowledge; “Gap” identifies unresolved evidence or work.
No completion percentage is inferred from these local labels.

Keep the full criterion text and its required justification/URL rules open
when entering answers. The linked file paths are evidence entrypoints, not
claims that every backend, package or historical run was audited.

The project website is [VMAFx on GitHub Pages](https://vmafx.github.io/vmafx/).
The published getting-started, CLI and API pages were checked on 2026-09-08;
the new homepage introduction and direct participation links are prepared in
[the documentation source](../index.md). Recheck the deployed homepage before
using that new text as evidence for `description_good` or `interact`.

## Criterion evidence map

### Basics

| Criterion | Requirement | Assessment | Evidence / remaining work |
| --- | --- | --- | --- |
| [`description_good`](https://www.bestpractices.dev/en/criteria/0#description_good) | MUST | Evidence | The [published CLI introduction](https://vmafx.github.io/vmafx/usage/cli/) explains comparing reference/distorted video and producing quality scores; the [public README](https://github.com/VMAFx/vmafx/blob/master/README.md) describes the project purpose. The shorter homepage introduction is a separate improvement pending deployment. |
| [`interact`](https://www.bestpractices.dev/en/criteria/0#interact) | MUST | Evidence | The [published getting-started guide](https://vmafx.github.io/vmafx/getting-started/), [public contribution guide](https://github.com/VMAFx/vmafx/blob/master/CONTRIBUTING.md) and [issue tracker](https://github.com/VMAFx/vmafx/issues) explain obtaining, reporting and contributing. New direct homepage links improve discovery; their deployment is still pending. |
| [`contribution`](https://www.bestpractices.dev/en/criteria/0#contribution) | MUST | Evidence | [CONTRIBUTING](../../CONTRIBUTING.md) documents branches, pull requests and review. |
| [`contribution_requirements`](https://www.bestpractices.dev/en/criteria/0#contribution_requirements) | SHOULD | Evidence | [Review expectations](../../CONTRIBUTING.md#review-expectations) and [principles](../principles.md) define contribution requirements; these are policies, not passing-run receipts. |
| [`floss_license`](https://www.bestpractices.dev/en/criteria/0#floss_license) | MUST | Evidence | [LICENSE](../../LICENSE) identifies BSD-2-Clause-Patent; the source is publicly available. Third-party components retain their own notices. |
| [`floss_license_osi`](https://www.bestpractices.dev/en/criteria/0#floss_license_osi) | SUGGESTED | Evidence | The root license is [OSI approved](https://opensource.org/license/bsd-2-clause-patent); no blanket claim is made for every optional external SDK. |
| [`license_location`](https://www.bestpractices.dev/en/criteria/0#license_location) | MUST | Evidence | [LICENSE](../../LICENSE) is at the repository root. |
| [`documentation_basics`](https://www.bestpractices.dev/en/criteria/0#documentation_basics) | MUST | Evidence | Published [installation](https://vmafx.github.io/vmafx/getting-started/), [CLI](https://vmafx.github.io/vmafx/usage/cli/), [MCP transport](https://vmafx.github.io/vmafx/mcp/http-transport/) and [AI security](https://vmafx.github.io/vmafx/ai/security/) guides cover installation, usage and relevant safety guidance. Sources: [getting started](../getting-started/index.md), [CLI](../usage/cli.md), [transport](../mcp/http-transport.md), [security](../ai/security.md). |
| [`documentation_interface`](https://www.bestpractices.dev/en/criteria/0#documentation_interface) | MUST | Evidence | Published [CLI](https://vmafx.github.io/vmafx/usage/cli/), [C API](https://vmafx.github.io/vmafx/api/) and [MCP tools](https://vmafx.github.io/vmafx/mcp/tools/) describe inputs and results. Sources: [CLI](../usage/cli.md), [API](../api/index.md), [tools](../mcp/tools.md). This is not an exhaustive doc-to-code coverage audit. |
| [`sites_https`](https://www.bestpractices.dev/en/criteria/0#sites_https) | MUST | Evidence | GitHub repository/releases and the [published docs](https://vmafx.github.io/vmafx/) respond over HTTPS. Future release download hosts need the same check. |
| [`discussion`](https://www.bestpractices.dev/en/criteria/0#discussion) | MUST | Evidence | [Issues](https://github.com/VMAFx/vmafx/issues) and [discussions](https://github.com/VMAFx/vmafx/discussions) are public, searchable web interfaces with addressable threads. |
| [`english`](https://www.bestpractices.dev/en/criteria/0#english) | SHOULD | Evidence | The README, guides and issue templates are in English; contribution guidance accepts public reports. |
| [`maintained`](https://www.bestpractices.dev/en/criteria/0#maintained) | MUST | Evidence | The active repository is not archived; ongoing [PR #1425](https://github.com/VMAFx/vmafx/pull/1425) and recent fixes document maintenance activity. |

### Change Control

| Criterion | Requirement | Assessment | Evidence / remaining work |
| --- | --- | --- | --- |
| [`repo_public`](https://www.bestpractices.dev/en/criteria/0#repo_public) | MUST | Evidence | [VMAFx/vmafx](https://github.com/VMAFx/vmafx) is public; do not use the archived repository identity. |
| [`repo_track`](https://www.bestpractices.dev/en/criteria/0#repo_track) | MUST | Evidence | Git commit objects record source differences, authors and timestamps. |
| [`repo_interim`](https://www.bestpractices.dev/en/criteria/0#repo_interim) | MUST | Evidence | [Pull requests](https://github.com/VMAFx/vmafx/pulls) expose interim source revisions before releases. |
| [`repo_distributed`](https://www.bestpractices.dev/en/criteria/0#repo_distributed) | SUGGESTED | Evidence | The repository uses Git. |
| [`version_unique`](https://www.bestpractices.dev/en/criteria/0#version_unique) | MUST | Evidence | Full Git commit IDs identify development revisions. The official criterion permits commit identifiers; this does not establish a published VMAFx release. |
| [`version_semver`](https://www.bestpractices.dev/en/criteria/0#version_semver) | SUGGESTED | Prepared | [Release policy](release.md) defines the independent VMAFx SemVer stream. First-release execution remains unverified. |
| [`version_tags`](https://www.bestpractices.dev/en/criteria/0#version_tags) | SUGGESTED | Prepared | [Release automation](../../.github/workflows/release-please.yml) plans release tags. No published VMAFx release was available to assess; inherited tags are not evidence. |
| [`release_notes`](https://www.bestpractices.dev/en/criteria/0#release_notes) | MUST | Prepared | [CHANGELOG](../../CHANGELOG.md) and [release guide](release.md) provide human summaries. Validate the first actual release notes; reusable software does not qualify for the single-service N/A exception. |
| [`release_notes_vulns`](https://www.bestpractices.dev/en/criteria/0#release_notes_vulns) | MUST | Unknown | No published VMAFx release is listed. Confirm advisory/CVE history before choosing an allowed N/A; absence of release assets alone is not a vulnerability-history review. |

### Reporting

| Criterion | Requirement | Assessment | Evidence / remaining work |
| --- | --- | --- | --- |
| [`report_process`](https://www.bestpractices.dev/en/criteria/0#report_process) | MUST | Evidence | [CONTRIBUTING](../../CONTRIBUTING.md#reporting-bugs--requesting-features) and [.github/ISSUE_TEMPLATE](../../.github/ISSUE_TEMPLATE/) explain reports. |
| [`report_tracker`](https://www.bestpractices.dev/en/criteria/0#report_tracker) | SHOULD | Evidence | [GitHub issues](https://github.com/VMAFx/vmafx/issues) track reports. |
| [`report_responses`](https://www.bestpractices.dev/en/criteria/0#report_responses) | MUST | Unknown | Measure acknowledgments over a specified 2–12-month bug-report cohort, with denominator and response links. Recent activity alone does not prove a majority. |
| [`enhancement_responses`](https://www.bestpractices.dev/en/criteria/0#enhancement_responses) | SHOULD | Unknown | Measure responses over the same declared enhancement-request cohort. Unmet SHOULD needs a reason, not a guessed percentage. |
| [`report_archive`](https://www.bestpractices.dev/en/criteria/0#report_archive) | MUST | Evidence | [All issues](https://github.com/VMAFx/vmafx/issues?q=is%3Aissue) retain reports and responses in a public searchable archive. |
| [`vulnerability_report_process`](https://www.bestpractices.dev/en/criteria/0#vulnerability_report_process) | MUST | Evidence | [SECURITY](../../SECURITY.md#reporting-a-vulnerability) already publishes the reporting route; the owner enabled GitHub private reporting and the API readback now confirms it. New support-policy wording remains pending publication. |
| [`vulnerability_report_private`](https://www.bestpractices.dev/en/criteria/0#vulnerability_report_private) | MUST | Evidence | [GitHub private reporting](https://github.com/VMAFx/vmafx/security/advisories/new) is now enabled, verified by API readback. The published policy documents this private channel; mailbox history and response performance are not implied. |
| [`vulnerability_report_response`](https://www.bestpractices.dev/en/criteria/0#vulnerability_report_response) | MUST | Unknown | A maintainer must check the last six months of private/public reports against the 14-day bound, or justify allowed N/A if none occurred. The 72-hour target is not history. |

### Quality

| Criterion | Requirement | Assessment | Evidence / remaining work |
| --- | --- | --- | --- |
| [`build`](https://www.bestpractices.dev/en/criteria/0#build) | MUST | Evidence | [Meson/Ninja build](../getting-started/index.md) and [Makefile](../../Makefile) rebuild source. Retained RC1 CPU controls establish that bounded profile; optional backend acceptance remains separate. |
| [`build_common_tools`](https://www.bestpractices.dev/en/criteria/0#build_common_tools) | SUGGESTED | Evidence | [core/meson.build](../../core/meson.build) and the Makefile use Meson, Ninja and standard compiler tools. |
| [`build_floss_tools`](https://www.bestpractices.dev/en/criteria/0#build_floss_tools) | SHOULD | Evidence | The documented CPU-only profile uses GCC/Clang, Meson and Ninja. This statement excludes optional proprietary SDK/backend requirements. |
| [`test`](https://www.bestpractices.dev/en/criteria/0#test) | MUST | Evidence | BSD-licensed [core tests](../../core/test/meson.build) and [documented commands](../../CONTRIBUTING.md) provide an automated test suite. |
| [`test_invocation`](https://www.bestpractices.dev/en/criteria/0#test_invocation) | SHOULD | Evidence | Use `meson test -C build`, `make test` and documented package-specific test entrypoints. |
| [`test_most`](https://www.bestpractices.dev/en/criteria/0#test_most) | SUGGESTED | Unknown | A configured coverage gate is not a current whole-project branch/input/functionality report. Retain uncovered backends and optional packages in the assessment. |
| [`test_continuous_integration`](https://www.bestpractices.dev/en/criteria/0#test_continuous_integration) | SUGGESTED | Evidence | [Tests workflow](../../.github/workflows/tests-and-quality-gates.yml) runs automated checks for changes; configured CI is not proof all current jobs pass. |
| [`test_policy`](https://www.bestpractices.dev/en/criteria/0#test_policy) | MUST | Prepared | [Review expectations](../../CONTRIBUTING.md#review-expectations) now explicitly require tests with major new functionality; publish this change before attesting the policy. |
| [`tests_are_added`](https://www.bestpractices.dev/en/criteria/0#tests_are_added) | MUST | Evidence | [Registration controls](../research/2047-option-aware-context-registration-2026-09-08.md) and [ownership controls](../research/2048-model-registration-ownership-2026-09-08.md) bind recent major changes to new regressions. Recheck the latest change set at submission time. |
| [`tests_documented_added`](https://www.bestpractices.dev/en/criteria/0#tests_documented_added) | SUGGESTED | Prepared | The same new contribution-policy paragraph tells contributors to add tests and document invocation. |
| [`warnings`](https://www.bestpractices.dev/en/criteria/0#warnings) | MUST | Evidence | [.clang-tidy](../../.clang-tidy), [configured analyzer](../../scripts/ci/lint-configured.py) and [lint workflow](../../.github/workflows/lint-and-format.yml) enable diagnostics. |
| [`warnings_fixed`](https://www.bestpractices.dev/en/criteria/0#warnings_fixed) | MUST | Gap | The RC1 baseline still contains 1,229 warnings and 44 uncited markers. The [ratchet](ci.md) records progress, but this assessment has not justified all accepted warnings or established full lint acceptance. |
| [`warnings_strict`](https://www.bestpractices.dev/en/criteria/0#warnings_strict) | SUGGESTED | Evidence | The strict analyzer configuration and exhaustive Cppcheck policy exist; remaining findings stay visible under warnings_fixed. |

### Security

| Criterion | Requirement | Assessment | Evidence / remaining work |
| --- | --- | --- | --- |
| [`know_secure_design`](https://www.bestpractices.dev/en/criteria/0#know_secure_design) | MUST | Unknown | A named primary developer must attest familiarity with every design principle in the official criterion. Agent activity, policy prose and tool results cannot establish a person's knowledge. |
| [`know_common_errors`](https://www.bestpractices.dev/en/criteria/0#know_common_errors) | MUST | Unknown | A named primary developer must attest relevant error classes and at least one mitigation for each; do not infer this from a codebase scan. |
| [`crypto_published`](https://www.bestpractices.dev/en/criteria/0#crypto_published) | MUST | Unknown | Inventory shipped security mechanisms and their actual default protocols/algorithms. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_call`](https://www.bestpractices.dev/en/criteria/0#crypto_call) | SHOULD | Unknown | Verify mechanisms delegate to established cryptographic libraries; examples alone do not cover the full project. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_floss`](https://www.bestpractices.dev/en/criteria/0#crypto_floss) | MUST | Unknown | Verify each cryptographic feature has a FLOSS implementation and dependency path. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_keylength`](https://www.bestpractices.dev/en/criteria/0#crypto_keylength) | MUST | Unknown | Check actual default key lengths and the ability to disable inadequate configurations. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_working`](https://www.bestpractices.dev/en/criteria/0#crypto_working) | MUST | Unknown | Check defaults for broken primitives/modes and document any permitted interoperability exception. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_weaknesses`](https://www.bestpractices.dev/en/criteria/0#crypto_weaknesses) | SHOULD | Unknown | Check weaker protocols/modes and justify any unmet SHOULD. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_pfs`](https://www.bestpractices.dev/en/criteria/0#crypto_pfs) | SHOULD | Unknown | Determine which shipped key-agreement paths apply and whether defaults provide forward secrecy. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_password_storage`](https://www.bestpractices.dev/en/criteria/0#crypto_password_storage) | MUST | Unknown | Determine whether any shipped service stores external-user passwords; only then choose a justified N/A or verify its password hashing. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`crypto_random`](https://www.bestpractices.dev/en/criteria/0#crypto_random) | MUST | Unknown | Verify key/nonce generation uses cryptographic randomness throughout applicable shipped services. The [MCP auth guide](../mcp/http-transport.md), [model verification](../ai/model-registry.md) and controller auth code mean a project-wide “no crypto” N/A cannot be assumed. |
| [`delivery_mitm`](https://www.bestpractices.dev/en/criteria/0#delivery_mitm) | MUST | Evidence | Source delivery uses [GitHub HTTPS](https://github.com/VMAFx/vmafx); Python dependencies are hash-locked with cryptographic SHA-256 digests ([ADR-1305](../adr/1305-hash-locked-python-installs.md)); future release-asset delivery must retain HTTPS and verified provenance where promised. |
| [`delivery_unsigned`](https://www.bestpractices.dev/en/criteria/0#delivery_unsigned) | MUST | Unknown | Shipped release assets and packages are not cryptographically signed (e.g. via Sigstore/cosign or GPG), and release checksums must be accompanied by signatures or verified delivery channels. Internal pip hash locks verify build inputs, not downstream delivered artifacts. |
| [`vulnerabilities_fixed_60_days`](https://www.bestpractices.dev/en/criteria/0#vulnerabilities_fixed_60_days) | MUST | Unknown | The corrected [policy](../../SECURITY.md#response-timeline) requires the public medium-or-higher 60-day bound. A dated advisory/triage inventory must establish actual compliance. |
| [`vulnerabilities_critical_fixed`](https://www.bestpractices.dev/en/criteria/0#vulnerabilities_critical_fixed) | SHOULD | Unknown | An urgent-remediation policy is present; actual critical-report history and closure/mitigation dates need maintainer confirmation. |
| [`no_leaked_credentials`](https://www.bestpractices.dev/en/criteria/0#no_leaked_credentials) | MUST | Unknown | [Gitleaks configuration](../../.github/workflows/security-scans.yml) and GitHub scanning are controls, not proof that no valid private credential exists in public history. Review live alerts and remediation evidence privately. |

### Analysis

| Criterion | Requirement | Assessment | Evidence / remaining work |
| --- | --- | --- | --- |
| [`static_analysis`](https://www.bestpractices.dev/en/criteria/0#static_analysis) | MUST | Prepared | [Local/CI analyzers](ci.md) and retained RC1 runs provide evidence of application. Complete and bind analysis to the actual proposed production release; do not use a configuration file as a successful release result. |
| [`static_analysis_common_vulnerabilities`](https://www.bestpractices.dev/en/criteria/0#static_analysis_common_vulnerabilities) | SUGGESTED | Evidence | [.clang-tidy](../../.clang-tidy) enables analyzer/CERT checks; [.semgrep.yml](../../.semgrep.yml) supplies additional project rules. |
| [`static_analysis_fixed`](https://www.bestpractices.dev/en/criteria/0#static_analysis_fixed) | MUST | Unknown | Confirm the disposition and dates of every confirmed medium-or-higher exploitable static finding. Diagnostic counts alone do not classify exploitability or remediation. |
| [`static_analysis_often`](https://www.bestpractices.dev/en/criteria/0#static_analysis_often) | SUGGESTED | Evidence | [Security](../../.github/workflows/security-scans.yml) and [lint](../../.github/workflows/lint-and-format.yml) configure analysis on changes; retain lane/impact skips and actual workflow status. |
| [`dynamic_analysis`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis) | SUGGESTED | Prepared | [Fuzzing](fuzzing.md) and retained sanitizer controls show mechanisms and bounded runs. A final release-wide dynamic-analysis receipt remains separate. |
| [`dynamic_analysis_unsafe`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis_unsafe) | SUGGESTED | Prepared | [Fuzz workflow](../../.github/workflows/fuzz.yml) configures scheduled C/C++ fuzz targets with ASan. Verify recurring run results; the schedule alone is not execution history. |
| [`dynamic_analysis_enable_assertions`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis_enable_assertions) | SUGGESTED | Evidence | The [fuzz build](../../.github/workflows/fuzz.yml) selects a debug ASan profile; [native tests](../../core/test/meson.build) retain assertions. Confirm the final run used those settings. |
| [`dynamic_analysis_fixed`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis_fixed) | MUST | Unknown | Confirm all medium-or-higher exploitable dynamic findings and their dispositions; sanitizer regressions do not establish absence of other findings. |

## Work that must precede a passing claim

1. Publish the corrected support and contribution policies. Preserve the
   now-verified private-reporting route and recheck it before attestation.
2. Bind release notes, static/dynamic analysis and supported artifacts to the
   actual first release. Resolve or explicitly assess outstanding diagnostics;
   do not interpret an analyzer ratchet as a clean scan.
3. Have a primary maintainer confirm the two developer-knowledge requirements,
   report-response cohorts and private vulnerability-response/remediation
   history. Keep sensitive reports private; a dated aggregate and reviewed
   conclusion can support the public answer without publishing report details.
4. Review applicable cryptographic and delivery paths and live credential
   alerts. Optional services and model verification are part of the project;
   the core metric's purpose does not make those criteria globally N/A.
5. Use the registered active-repository record, submit only reviewed answers, and
   read back the resulting project ID and badge status. An in-progress badge
   is useful and must not be presented as passing.

## Enrollment and API handoff

[BadgeApp API documentation](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/docs/api.md)
uses `.json` URL suffixes, not the HTTP `Accept` header. Read-only queries include
`/projects.json?url=https%3A%2F%2Fgithub.com%2FVMAFx%2Fvmafx`,
`/projects/ID.json` and `/projects/ID/badge.json`. The initial exact repository-URL query returned no match; the owner then
registered project **14549**. Its public JSON readback identifies the correct
repository and `in_progress` badge level. Use that record; recheck identity and
persisted answers before changing it.

The checked-in [OpenAPI document](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/best_practices.openapi.yaml)
describes read paths and a minimal project shape; it does not specify a complete
write payload. The API guide documents logged-in-session creation/update routes.
Criterion fields use `<id>_status` and `<id>_justification`; external statuses are
`?`, `Unmet`, `N/A` and `Met`. The official
[proposal-file mechanism](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/docs/bestpractices-json.md)
ignores unknown answers. Do not turn this worksheet into automatic affirmative
answers or guess authentication requirements from the read-only schema.
Account authorization and external submission are separate maintainer actions.

Criterion identifiers and categories are attributed to the OpenSSF Best
Practices badge contributors; their content is available under CC-BY-3.0+.
[Research-2055](../research/2055-best-practices-passing-evidence.md) records source
revisions, live-check limits and the corrections made with this assessment.
