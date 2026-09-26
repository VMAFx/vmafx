<!-- markdownlint-disable MD013 MD060 -->
# OpenSSF Best Practices passing badge

VMAFx holds the OpenSSF Best Practices **passing** badge as
[project 14549](https://www.bestpractices.dev/en/projects/14549). The badge is
voluntary self-certification: an authorized maintainer answers the 67
[passing criteria](https://www.bestpractices.dev/en/criteria/0), and the
project record, not this page, is authoritative.

The public readback of `https://www.bestpractices.dev/projects/14549.json` on
2026-09-26 shows `badge_level` `passing`, reached at
`2026-09-26T21:06:31.870Z`, with every passing criterion answered: 63 `Met`,
3 `N/A` and 1 `Unmet` (`version_tags`, a SUGGESTED criterion). The same
readback puts silver at 15% and gold at 22%. Scorecard's `CII-Best-Practices`
check scores a passing badge 5 out of 10
([`cii_best_practices.go`](https://github.com/ossf/scorecard/blob/main/checks/evaluation/cii_best_practices.go));
the published Scorecard result changes on the next master scan.

This page mirrors that submission so each answer can be checked against the
repository. The 33 criteria answered on 2026-09-08 were reviewed at the RC1
candidate `76a7c467c3524478c25d588a31b37b2d45d96081`. The 34 answered on
2026-09-26 were reviewed at master `52ead780c`; `crypto_keylength` also relies
on the controller's RSA key-size check from
[PR #1566](https://github.com/VMAFx/vmafx/pull/1566). No VMAFx release was
published at either date, and inherited libvmaf `3.x` identifiers are not
VMAFx support or release evidence.

## How to read the tables

The official [achievement rules](https://www.bestpractices.dev/en/criteria_discussion#achieving-a-badge)
require all MUST/MUST NOT criteria, a result or justified exception for every
SHOULD, and consideration of every SUGGESTED item. N/A is allowed only where
specified and sometimes requires justification.

**Submitted** is the status stored in the project record at the readback above.
**Evidence** is the submitted justification for the criteria answered on
2026-09-26. For the criteria answered on 2026-09-08 it is the review note from
that date; the justification actually submitted for them is in the project
record. Links are evidence entry points, not claims that every backend, package
or historical run was audited.

The project website is [VMAFx on GitHub Pages](https://vmafx.github.io/vmafx/).

## Criterion evidence map

### Basics

| Criterion | Requirement | Submitted | Evidence |
| --- | --- | --- | --- |
| [`description_good`](https://www.bestpractices.dev/en/criteria/0#description_good) | MUST | Met | The [published CLI introduction](https://vmafx.github.io/vmafx/usage/cli/) explains comparing reference/distorted video and producing quality scores; the [public README](https://github.com/VMAFx/vmafx/blob/master/README.md) describes the project purpose. The shorter homepage introduction is a separate improvement pending deployment. |
| [`interact`](https://www.bestpractices.dev/en/criteria/0#interact) | MUST | Met | The [published getting-started guide](https://vmafx.github.io/vmafx/getting-started/), [public contribution guide](https://github.com/VMAFx/vmafx/blob/master/CONTRIBUTING.md) and [issue tracker](https://github.com/VMAFx/vmafx/issues) explain obtaining, reporting and contributing. New direct homepage links improve discovery; their deployment is still pending. |
| [`contribution`](https://www.bestpractices.dev/en/criteria/0#contribution) | MUST | Met | [CONTRIBUTING](../../CONTRIBUTING.md) documents branches, pull requests and review. |
| [`contribution_requirements`](https://www.bestpractices.dev/en/criteria/0#contribution_requirements) | SHOULD | Met | [Review expectations](../../CONTRIBUTING.md#review-expectations) and [principles](../principles.md) define contribution requirements; these are policies, not passing-run receipts. |
| [`floss_license`](https://www.bestpractices.dev/en/criteria/0#floss_license) | MUST | Met | [LICENSE](../../LICENSE) identifies BSD-2-Clause-Patent; the source is publicly available. Third-party components retain their own notices. |
| [`floss_license_osi`](https://www.bestpractices.dev/en/criteria/0#floss_license_osi) | SUGGESTED | Met | The root license is [OSI approved](https://opensource.org/license/bsd-2-clause-patent); no blanket claim is made for every optional external SDK. |
| [`license_location`](https://www.bestpractices.dev/en/criteria/0#license_location) | MUST | Met | [LICENSE](../../LICENSE) is at the repository root. |
| [`documentation_basics`](https://www.bestpractices.dev/en/criteria/0#documentation_basics) | MUST | Met | Published [installation](https://vmafx.github.io/vmafx/getting-started/), [CLI](https://vmafx.github.io/vmafx/usage/cli/), [MCP transport](https://vmafx.github.io/vmafx/mcp/http-transport/) and [AI security](https://vmafx.github.io/vmafx/ai/security/) guides cover installation, usage and relevant safety guidance. Sources: [getting started](../getting-started/index.md), [CLI](../usage/cli.md), [transport](../mcp/http-transport.md), [security](../ai/security.md). |
| [`documentation_interface`](https://www.bestpractices.dev/en/criteria/0#documentation_interface) | MUST | Met | Published [CLI](https://vmafx.github.io/vmafx/usage/cli/), [C API](https://vmafx.github.io/vmafx/api/) and [MCP tools](https://vmafx.github.io/vmafx/mcp/tools/) describe inputs and results. Sources: [CLI](../usage/cli.md), [API](../api/index.md), [tools](../mcp/tools.md). This is not an exhaustive doc-to-code coverage audit. |
| [`sites_https`](https://www.bestpractices.dev/en/criteria/0#sites_https) | MUST | Met | GitHub repository/releases and the [published docs](https://vmafx.github.io/vmafx/) respond over HTTPS. Future release download hosts need the same check. |
| [`discussion`](https://www.bestpractices.dev/en/criteria/0#discussion) | MUST | Met | [Issues](https://github.com/VMAFx/vmafx/issues) and [discussions](https://github.com/VMAFx/vmafx/discussions) are public, searchable web interfaces with addressable threads. |
| [`english`](https://www.bestpractices.dev/en/criteria/0#english) | SHOULD | Met | The README, guides and issue templates are in English; contribution guidance accepts public reports. |
| [`maintained`](https://www.bestpractices.dev/en/criteria/0#maintained) | MUST | Met | The active repository is not archived; ongoing [PR #1425](https://github.com/VMAFx/vmafx/pull/1425) and recent fixes document maintenance activity. |

### Change Control

| Criterion | Requirement | Submitted | Evidence |
| --- | --- | --- | --- |
| [`repo_public`](https://www.bestpractices.dev/en/criteria/0#repo_public) | MUST | Met | [VMAFx/vmafx](https://github.com/VMAFx/vmafx) is public; do not use the archived repository identity. |
| [`repo_track`](https://www.bestpractices.dev/en/criteria/0#repo_track) | MUST | Met | Git commit objects record source differences, authors and timestamps. |
| [`repo_interim`](https://www.bestpractices.dev/en/criteria/0#repo_interim) | MUST | Met | [Pull requests](https://github.com/VMAFx/vmafx/pulls) expose interim source revisions before releases. |
| [`repo_distributed`](https://www.bestpractices.dev/en/criteria/0#repo_distributed) | SUGGESTED | Met | The repository uses Git. |
| [`version_unique`](https://www.bestpractices.dev/en/criteria/0#version_unique) | MUST | Met | Full Git commit IDs identify development revisions. The official criterion permits commit identifiers; this does not establish a published VMAFx release. |
| [`version_semver`](https://www.bestpractices.dev/en/criteria/0#version_semver) | SUGGESTED | Met | [Release policy](release.md) defines the independent VMAFx SemVer stream. First-release execution remains unverified. |
| [`version_tags`](https://www.bestpractices.dev/en/criteria/0#version_tags) | SUGGESTED | Unmet | No VMAFx release has been published yet (<https://github.com/VMAFx/vmafx/releases> is empty), so no VMAFx release tag exists. The v1.x-v3.x tags in the repository are inherited Netflix/vmaf upstream releases. release-please creates a vX.Y.Z git tag for each release when its draft is published (<https://github.com/VMAFx/vmafx/blob/master/docs/development/release.md>). |
| [`release_notes`](https://www.bestpractices.dev/en/criteria/0#release_notes) | MUST | Met | [CHANGELOG](../../CHANGELOG.md) and [release guide](release.md) provide human summaries. Validate the first actual release notes; reusable software does not qualify for the single-service N/A exception. |
| [`release_notes_vulns`](https://www.bestpractices.dev/en/criteria/0#release_notes_vulns) | MUST | N/A | N/A: VMAFx has not published a release yet (<https://github.com/VMAFx/vmafx/releases>). No publicly known vulnerability with a CVE or similar ID exists for VMAFx or upstream libvmaf: the repository has no security advisories (<https://github.com/VMAFx/vmafx/security/advisories>), and NVD lists no VMAF CVE. The security policy commits to naming fixes in release notes (<https://github.com/VMAFx/vmafx/blob/master/SECURITY.md#response-timeline>). |

### Reporting

| Criterion | Requirement | Submitted | Evidence |
| --- | --- | --- | --- |
| [`report_process`](https://www.bestpractices.dev/en/criteria/0#report_process) | MUST | Met | [CONTRIBUTING](../../CONTRIBUTING.md#reporting-bugs--requesting-features) and [.github/ISSUE_TEMPLATE](../../.github/ISSUE_TEMPLATE/) explain reports. |
| [`report_tracker`](https://www.bestpractices.dev/en/criteria/0#report_tracker) | SHOULD | Met | [GitHub issues](https://github.com/VMAFx/vmafx/issues) track reports. |
| [`report_responses`](https://www.bestpractices.dev/en/criteria/0#report_responses) | MUST | Met | Window: 2025-09-26 to 2026-07-26 (2-12 months before this answer; the repository was created on 2026-05-28). The tracker received no bug reports from anyone other than the maintainer in that window. The only outside submissions were pull requests #1081 (a SYCL bug fix) and #1082, and both were answered and merged (<https://github.com/VMAFx/vmafx/pull/1081>, <https://github.com/VMAFx/vmafx/pull/1082>). All reports stay public at <https://github.com/VMAFx/vmafx/issues?q=is%3Aissue>. |
| [`enhancement_responses`](https://www.bestpractices.dev/en/criteria/0#enhancement_responses) | SHOULD | Met | Every genuine enhancement request in the last 2-12 months got a response. The only outside one, PR #1082 (the VMAF v1 model-port research digest and ADR), was answered and merged (<https://github.com/VMAFx/vmafx/pull/1082>). The only other third-party issue, #1225, was unsolicited automated solicitation and was answered and closed within 8 hours (<https://github.com/VMAFx/vmafx/issues/1225>). |
| [`report_archive`](https://www.bestpractices.dev/en/criteria/0#report_archive) | MUST | Met | [All issues](https://github.com/VMAFx/vmafx/issues?q=is%3Aissue) retain reports and responses in a public searchable archive. |
| [`vulnerability_report_process`](https://www.bestpractices.dev/en/criteria/0#vulnerability_report_process) | MUST | Met | [SECURITY](../../SECURITY.md#reporting-a-vulnerability) already publishes the reporting route; the owner enabled GitHub private reporting and the API readback now confirms it. New support-policy wording remains pending publication. |
| [`vulnerability_report_private`](https://www.bestpractices.dev/en/criteria/0#vulnerability_report_private) | MUST | Met | [GitHub private reporting](https://github.com/VMAFx/vmafx/security/advisories/new) is now enabled, verified by API readback. The published policy documents this private channel; mailbox history and response performance are not implied. |
| [`vulnerability_report_response`](https://www.bestpractices.dev/en/criteria/0#vulnerability_report_response) | MUST | N/A | N/A: no vulnerability report was received in the last 6 months. GitHub private vulnerability reporting is enabled and has produced no reports or draft advisories (<https://github.com/VMAFx/vmafx/security/advisories>). No report arrived through the e-mail channel listed in <https://github.com/VMAFx/vmafx/blob/master/SECURITY.md>, whose published target is acknowledgement within 72 hours. |

### Quality

| Criterion | Requirement | Submitted | Evidence |
| --- | --- | --- | --- |
| [`build`](https://www.bestpractices.dev/en/criteria/0#build) | MUST | Met | [Meson/Ninja build](../getting-started/index.md) and [Makefile](../../Makefile) rebuild source. Retained RC1 CPU controls establish that bounded profile; optional backend acceptance remains separate. |
| [`build_common_tools`](https://www.bestpractices.dev/en/criteria/0#build_common_tools) | SUGGESTED | Met | [core/meson.build](../../core/meson.build) and the Makefile use Meson, Ninja and standard compiler tools. |
| [`build_floss_tools`](https://www.bestpractices.dev/en/criteria/0#build_floss_tools) | SHOULD | Met | The default build uses only FLOSS tools: GCC or Clang, Meson, Ninja and Python (<https://vmafx.github.io/vmafx/getting-started/>, <https://github.com/VMAFx/vmafx/blob/master/core/meson.build>). CUDA, SYCL and HIP are off by default. Metal is detected only on macOS; only those optional backends need a vendor toolchain. |
| [`test`](https://www.bestpractices.dev/en/criteria/0#test) | MUST | Met | BSD-licensed [core tests](../../core/test/meson.build) and [documented commands](../../CONTRIBUTING.md) provide an automated test suite. |
| [`test_invocation`](https://www.bestpractices.dev/en/criteria/0#test_invocation) | SHOULD | Met | Use `make test`, the credential-safe Meson runner, and documented package-specific test entrypoints. |
| [`test_most`](https://www.bestpractices.dev/en/criteria/0#test_most) | SUGGESTED | Met | The Coverage Gate measures the CPU libvmaf build on every change. On master 52ead780c it exercises 79.5% of lines (28,565/35,935) and 62.5% of branches (<https://github.com/VMAFx/vmafx/actions/runs/36268143869/job/108476747662>). The GPU backends and the Go and Python packages have their own suites, which are not in this figure. |
| [`test_continuous_integration`](https://www.bestpractices.dev/en/criteria/0#test_continuous_integration) | SUGGESTED | Met | GitHub Actions runs the build, unit tests, Netflix golden-data tests, sanitizers, coverage, lint and security scans on every pull request and every push to master (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/tests-and-quality-gates.yml>, <https://github.com/VMAFx/vmafx/actions>). A required aggregate CI status check gates merges to master. |
| [`test_policy`](https://www.bestpractices.dev/en/criteria/0#test_policy) | MUST | Met | The contribution guide requires major new functionality to include automated tests in the same change, covering the new behaviour and its failure paths. It also tells reviewers to check the tests against the actual change (<https://github.com/VMAFx/vmafx/blob/master/CONTRIBUTING.md#review-expectations>). |
| [`tests_are_added`](https://www.bestpractices.dev/en/criteria/0#tests_are_added) | MUST | Met | Recent major changes added tests together with the functionality. Examples: percentile temporal pooling in the C API (<https://github.com/VMAFx/vmafx/pull/1340>, <https://github.com/VMAFx/vmafx/pull/1392>), the MCP bridge to the gRPC control plane (<https://github.com/VMAFx/vmafx/pull/1319>), the uncapped PSNR option (<https://github.com/VMAFx/vmafx/pull/1338>) and SYCL chroma MS-SSIM (<https://github.com/VMAFx/vmafx/pull/1523>). |
| [`tests_documented_added`](https://www.bestpractices.dev/en/criteria/0#tests_documented_added) | SUGGESTED | Met | The test-addition policy is part of the change-proposal instructions in <https://github.com/VMAFx/vmafx/blob/master/CONTRIBUTING.md#review-expectations>. The pull-request template asks for passing unit tests and a reproducer command (<https://github.com/VMAFx/vmafx/blob/master/.github/PULL_REQUEST_TEMPLATE.md>). |
| [`warnings`](https://www.bestpractices.dev/en/criteria/0#warnings) | MUST | Met | [.clang-tidy](../../.clang-tidy), [configured analyzer](../../scripts/ci/lint-configured.py) and [lint workflow](../../.github/workflows/lint-and-format.yml) enable diagnostics. |
| [`warnings_fixed`](https://www.bestpractices.dev/en/criteria/0#warnings_fixed) | MUST | Met | Warnings are fixed or explicitly justified. C/C++ builds use -Wall -Wextra (Meson warning_level=2). A whole-tree clang-tidy ratchet stops any file from gaining warnings and requires a lower baseline whenever a file is cleaned (<https://github.com/VMAFx/vmafx/blob/master/scripts/ci/tidy-ratchet.py>, <https://vmafx.github.io/vmafx/development/ci/>). The remaining strict-profile findings are about 0.9 per 100 lines across the five backend lanes (per-file counts in <https://github.com/VMAFx/vmafx/tree/master/scripts/ci>), and every in-source suppression states its reason. |
| [`warnings_strict`](https://www.bestpractices.dev/en/criteria/0#warnings_strict) | SUGGESTED | Met | The strict analyzer configuration and exhaustive Cppcheck policy exist; remaining findings stay visible under warnings_fixed. |

### Security

| Criterion | Requirement | Submitted | Evidence |
| --- | --- | --- | --- |
| [`know_secure_design`](https://www.bestpractices.dev/en/criteria/0#know_secure_design) | MUST | Met | The primary developer (GitHub: lusoris) knows the secure-design principles listed in this criterion and applies them. Fail-safe defaults: the MCP HTTP transport binds to 127.0.0.1 and rejects every request with 401 until a token is configured (<https://vmafx.github.io/vmafx/mcp/http-transport/>). Complete mediation and least privilege: each request gets an RS256 JWT check with tenant isolation and role-based access (<https://vmafx.github.io/vmafx/server/auth/>). Small attack surface: model verification fails closed and runs cosign without a shell (<https://vmafx.github.io/vmafx/ai/security/>). |
| [`know_common_errors`](https://www.bestpractices.dev/en/criteria/0#know_common_errors) | MUST | Met | The primary developer knows the error classes that affect a C/C++ media-parsing library and its network services, with at least one mitigation for each. Classes: buffer overflows, integer overflow, use-after-free, NULL dereference, command injection, path traversal, missing authentication or authorization, and timing leaks. Mitigations: SEI CERT C and Power-of-10 rules with banned unsafe functions (<https://vmafx.github.io/vmafx/principles/>); ASan, UBSan, TSan and libFuzzer in CI (<https://vmafx.github.io/vmafx/development/fuzzing/>); posix_spawnp instead of system(); path allowlists; deny-by-default authentication; constant-time token comparison. |
| [`crypto_published`](https://www.bestpractices.dev/en/criteria/0#crypto_published) | MUST | Met | VMAFx uses only published, expert-reviewed cryptography: RS256 JWT signatures (RSASSA-PKCS1-v1_5 with SHA-256, RFC 7518) in the controller (<https://github.com/VMAFx/vmafx/blob/master/cmd/vmafx-controller/auth/rsa.go>); TLS 1.2/1.3 for the optional MCP HTTPS listener (<https://github.com/VMAFx/vmafx/blob/master/mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py>); Sigstore/cosign signature checks for tiny-AI models (<https://github.com/VMAFx/vmafx/blob/master/core/src/dnn/model_loader.c>). No proprietary or home-grown algorithm is used. |
| [`crypto_call`](https://www.bestpractices.dev/en/criteria/0#crypto_call) | SHOULD | Met | VMAFx does not implement cryptographic primitives itself. It calls Go's standard crypto packages (crypto/rsa, crypto/sha256, crypto/rand, crypto/subtle), Python's ssl, hmac and hashlib modules (OpenSSL), and the cosign CLI for Sigstore verification (<https://github.com/VMAFx/vmafx/blob/master/cmd/vmafx-controller/auth/rsa.go>, <https://github.com/VMAFx/vmafx/blob/master/core/src/dnn/model_loader.c>). |
| [`crypto_floss`](https://www.bestpractices.dev/en/criteria/0#crypto_floss) | MUST | Met | Every cryptographic function is provided by FLOSS: the Go standard library (BSD-3-Clause), CPython's ssl, hmac and hashlib on OpenSSL (Apache-2.0), and Sigstore cosign (Apache-2.0). No proprietary cryptographic component is required. |
| [`crypto_keylength`](https://www.bestpractices.dev/en/criteria/0#crypto_keylength) | MUST | Met | Defaults meet NIST 2030 minimums and weaker keys are refused: the controller rejects JWKS RSA keys shorter than 2048 bits and malformed public exponents (<https://github.com/VMAFx/vmafx/blob/master/cmd/vmafx-controller/auth/middleware.go>, documented at <https://vmafx.github.io/vmafx/server/auth/>); node session tokens are 128-bit crypto/rand values; the optional MCP TLS listener uses OpenSSL security level 2 (at least 2048-bit RSA/DH, at least 224-bit ECC); model signatures use ECDSA P-256 via cosign. |
| [`crypto_working`](https://www.bestpractices.dev/en/criteria/0#crypto_working) | MUST | Met | The default security mechanisms use no broken algorithms or unsuitable modes. JWT verification accepts only RS256 and rejects alg=none and HS256 before key lookup (<https://vmafx.github.io/vmafx/server/auth/>). Python's ssl defaults on the optional MCP TLS listener allow only TLS 1.2 or later, with forward-secret suites that do not use SHA-1 MACs. Cosign checks use ECDSA P-256 with SHA-256. MD4, MD5, DES, RC4 and Dual_EC_DRBG are not used; SHA-1 appears only as a non-security cache key in the inherited Python harness. |
| [`crypto_weaknesses`](https://www.bestpractices.dev/en/criteria/0#crypto_weaknesses) | SHOULD | Met | No default security mechanism depends on SHA-1 or another algorithm or mode with serious known weaknesses. Signatures use SHA-256 (RS256, Sigstore), and token comparison is constant-time. Python 3.10+ ssl defaults disable TLS suites with SHA-1 MACs or without forward secrecy (<https://docs.python.org/3.10/whatsnew/3.10.html#ssl>). The SHA-1 calls in the inherited Python harness only name cached result files (<https://github.com/VMAFx/vmafx/blob/master/compat/python-vmaf/core/result_store.py>). |
| [`crypto_pfs`](https://www.bestpractices.dev/en/criteria/0#crypto_pfs) | SHOULD | Met | The only key-agreement protocol VMAFx runs itself is TLS on the optional MCP HTTPS listener. That listener uses Python's default server context: TLS 1.3, or TLS 1.2 limited to ECDHE/DHE suites, so every session has forward secrecy (<https://github.com/VMAFx/vmafx/blob/master/mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py>). The Go services do not implement TLS or any other key agreement. |
| [`crypto_password_storage`](https://www.bestpractices.dev/en/criteria/0#crypto_password_storage) | MUST | N/A | N/A: no VMAFx component stores passwords of external users. The controller authenticates users with JWTs issued by an external OIDC provider (<https://vmafx.github.io/vmafx/server/auth/>). The MCP HTTP transport compares one operator-supplied bearer token taken from the environment (<https://vmafx.github.io/vmafx/mcp/http-transport/>). |
| [`crypto_random`](https://www.bestpractices.dev/en/criteria/0#crypto_random) | MUST | Met | Security-relevant random values come from cryptographically secure generators. Controller node session tokens are 128-bit values from Go's crypto/rand (<https://github.com/VMAFx/vmafx/blob/master/cmd/vmafx-controller/nodes/registry.go>). TLS and Sigstore key and nonce generation is left to OpenSSL and cosign. math/rand is used only by a non-security parameter search (<https://github.com/VMAFx/vmafx/blob/master/pkg/prefilter/tpe.go>). |
| [`delivery_mitm`](https://www.bestpractices.dev/en/criteria/0#delivery_mitm) | MUST | Met | Source delivery uses [GitHub HTTPS](https://github.com/VMAFx/vmafx); Python dependencies are hash-locked with cryptographic SHA-256 digests ([ADR-1305](../adr/1305-hash-locked-python-installs.md)); future release-asset delivery must retain HTTPS and verified provenance where promised. |
| [`delivery_unsigned`](https://www.bestpractices.dev/en/criteria/0#delivery_unsigned) | MUST | Met | VMAFx never retrieves a cryptographic hash over plain HTTP. GitHub serves the source and release assets over HTTPS only. Python dependency hashes are committed in the repository's lock files, and pip verifies them (<https://github.com/VMAFx/vmafx/blob/master/python/requirements-lock.txt>). The release workflow signs every release asset keylessly with Sigstore cosign and publishes SLSA provenance (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/supply-chain.yml>). |
| [`vulnerabilities_fixed_60_days`](https://www.bestpractices.dev/en/criteria/0#vulnerabilities_fixed_60_days) | MUST | Met | There is no publicly known vulnerability in VMAFx: the repository has no security advisories (<https://github.com/VMAFx/vmafx/security/advisories>), and no CVE names VMAF or libvmaf. Vulnerable dependencies are also updated within 60 days. For example, the Go advisories for kin-openapi and gRPC were fixed by <https://github.com/VMAFx/vmafx/pull/1088> and <https://github.com/VMAFx/vmafx/pull/1089>, 8-36 days after the alerts opened. |
| [`vulnerabilities_critical_fixed`](https://www.bestpractices.dev/en/criteria/0#vulnerabilities_critical_fixed) | SHOULD | Met | No critical vulnerability has been reported in VMAFx. The security policy requires urgent handling of Critical reports, with a fix target of 30 days or less (<https://github.com/VMAFx/vmafx/blob/master/SECURITY.md#response-timeline>). |
| [`no_leaked_credentials`](https://www.bestpractices.dev/en/criteria/0#no_leaked_credentials) | MUST | Met | Gitleaks scans the full git history on every push and pull request and fails the build on any finding; the scan passes on master (<https://github.com/VMAFx/vmafx/actions/runs/36268143884>). GitHub secret scanning with push protection is enabled. Its only alert was a test fixture that grants access to nothing: the base64 of "user:pass" in a negative authentication test (<https://github.com/VMAFx/vmafx/blob/master/cmd/vmafx-controller/auth/grpc_interceptor_test.go>). Release signing is keyless, so there is no signing key to leak. |

### Analysis

| Criterion | Requirement | Submitted | Evidence |
| --- | --- | --- | --- |
| [`static_analysis`](https://www.bestpractices.dev/en/criteria/0#static_analysis) | MUST | Met | Static analysis runs on every pull request and every push to master, and releases are cut from master, so each proposed release is analysed. Tools: CodeQL (C/C++, Python, GitHub Actions) and Semgrep (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/security-scans.yml>); clang-tidy with clang-analyzer and CERT checks, plus cppcheck (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/lint-and-format.yml>); go vet and gosec (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/go-ci.yml>). All passed on master 52ead780c (<https://github.com/VMAFx/vmafx/actions/runs/36268143884>). |
| [`static_analysis_common_vulnerabilities`](https://www.bestpractices.dev/en/criteria/0#static_analysis_common_vulnerabilities) | SUGGESTED | Met | [.clang-tidy](../../.clang-tidy) enables analyzer/CERT checks; [.semgrep.yml](../../.semgrep.yml) supplies additional project rules. |
| [`static_analysis_fixed`](https://www.bestpractices.dev/en/criteria/0#static_analysis_fixed) | MUST | Met | All confirmed static-analysis findings are fixed. Master has no open CodeQL, Semgrep or Gitleaks alert; the only open code-scanning entries are four OpenSSF Scorecard repository-settings checks, which are not code findings. Findings closed without a code change carry a written dismissal: a false positive with proof that the operands are bounded, or test-only code. Fixes are logged in <https://github.com/VMAFx/vmafx/blob/master/docs/state.md>. |
| [`static_analysis_often`](https://www.bestpractices.dev/en/criteria/0#static_analysis_often) | SUGGESTED | Met | CodeQL, Semgrep, clang-tidy, cppcheck, go vet and gosec run on every push and pull request; an impact planner skips lanes a change cannot affect. The security scan also runs on a weekly schedule (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/security-scans.yml>). |
| [`dynamic_analysis`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis) | SUGGESTED | Met | Every change to the C core runs the native test suite under AddressSanitizer plus UndefinedBehaviorSanitizer and under ThreadSanitizer, daily as well (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/sanitizers.yml>). Five libFuzzer harnesses run nightly with ASan (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/fuzz.yml>). Releases are cut from master, where these pass (<https://github.com/VMAFx/vmafx/actions/runs/36268143829>). |
| [`dynamic_analysis_unsafe`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis_unsafe) | SUGGESTED | Met | The C/C++ code is fuzzed nightly with libFuzzer and AddressSanitizer across five harnesses: Y4M and YUV input, CLI parsing, JSON model loading and DNN sidecar parsing (<https://vmafx.github.io/vmafx/development/fuzzing/>). The test suite runs under ASan, UBSan and TSan on every change (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/sanitizers.yml>). |
| [`dynamic_analysis_enable_assertions`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis_enable_assertions) | SUGGESTED | Met | Sanitizer and fuzz builds use Meson's debug build type, so assert() stays enabled (NDEBUG is not defined), and the sanitizers abort on the first error (halt_on_error=1) (<https://github.com/VMAFx/vmafx/blob/master/.github/workflows/sanitizers.yml>, <https://github.com/VMAFx/vmafx/blob/master/.github/workflows/fuzz.yml>). A separate CI job enforces assertion density in the C core. |
| [`dynamic_analysis_fixed`](https://www.bestpractices.dev/en/criteria/0#dynamic_analysis_fixed) | MUST | Met | Memory-safety defects found by fuzzing and sanitizers are fixed within days, and their reproducers are kept as regression inputs. Examples: a NULL dereference on negative Y4M dimensions, a 4:1:1 chroma heap overflow, and a JSON-model heap overflow found by fuzz_json_model (<https://vmafx.github.io/vmafx/state/>, <https://github.com/VMAFx/vmafx/tree/master/core/test/fuzz>). No sanitizer or fuzzer finding is open. |

## Keeping the badge current

1. When the first VMAFx release is published, change `version_tags` to `Met`
   with a link to its tag, and point `release_notes`, `release_notes_vulns` and
   `version_semver` at that release instead of the policy documents.
2. `crypto_keylength` depends on the controller refusing JWKS RSA keys below
   2048 bits ([controller auth](../server/auth.md#oidc-provider-configuration)).
   Weakening that check makes the answer false.
3. `report_responses`, `enhancement_responses` and
   `vulnerability_report_response` describe rolling windows. Re-measure them
   when the record is next edited, and at least with each release.
4. OpenSSF revises the criteria from time to time; a new criterion starts
   unanswered and can drop the badge level. Recheck the project JSON after a
   criteria change is announced.
5. Change answers only through the logged-in project form, read back
   `/projects/14549.json`, and update this page in the same change.

## Enrollment and API handoff

[BadgeApp API documentation](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/docs/api.md)
uses `.json` URL suffixes, not the HTTP `Accept` header. Read-only queries include
`/projects.json?url=https%3A%2F%2Fgithub.com%2FVMAFx%2Fvmafx`,
`/projects/ID.json` and `/projects/ID/badge.json`. The initial exact repository-URL query returned no match; the owner then
registered project **14549**. Its public JSON readback identifies the correct
repository and, since 2026-09-26, the `passing` badge level. Use that record;
recheck identity and persisted answers before changing it.

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
