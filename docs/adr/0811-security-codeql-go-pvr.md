<!-- markdownlint-disable MD013 MD060 -->
# ADR-0811: Security hardening — CodeQL Go coverage + codeql-config

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `security`, `codeql`, `go`, `dependabot`, `ossf`

## Context

A security audit (2026-05-29) found three gaps:

1. **Stale paths in `.github/codeql-config.yml`**: The file contained an
   unresolved Git merge conflict left from the `libvmaf/ → core/` rename
   (ADR-0700). Both HEAD and the conflicting branch had valid partial path
   updates; neither set was complete. The conflict caused the CodeQL C/C++
   and Python jobs to consume a syntactically-broken config file; behaviour
   was undefined (GitHub CodeQL silently ignores malformed config and falls
   back to scanning everything, which may include generated or test files
   that inflate noise).

2. **No CodeQL coverage for Go**: Phase 4 language modernization (ADR-0708
   area) added substantial Go code under `cmd/`, `pkg/`, and `api/`. The
   `security-scans.yml` workflow covered C/C++, Python, and Actions but
   omitted Go entirely. The Go surface includes the `vmafx-controller`
   HTTP/gRPC server, `vmafx-mcp` binary, and `pkg/ai/infer.go` (ONNX
   inference gateway) — all security-relevant attack surfaces.

3. **Dependabot vulnerability alerts disabled**: The private repo had
   Dependabot alerts disabled (HTTP 422 from the API), consistent with the
   paid-plan requirement. Renovate (`renovate.json`) is the active
   dependency-update mechanism (Dependabot was superseded by ADR-0363), and
   Renovate has `osvVulnerabilityAlerts: true` configured. This is an
   acceptable mitigating control; no code change is required, but the audit
   finding is documented here.

## Decision

1. Resolve the merge conflict in `.github/codeql-config.yml`, adopting the
   HEAD side (`core/` layout) in full, and extend the `paths:` list with
   `cmd`, `pkg`, and `api` (the new Go surface). Exclude `gen/go`
   (generated protobuf stubs).

2. Add a `codeql-go` job to `.github/workflows/security-scans.yml` covering
   `cmd/`, `pkg/`, and `api/` with the `security-and-quality` query suite.
   The job is gated by the same draft-PR guard as all other security jobs.

3. Document the Dependabot/Renovate posture: Renovate with
   `osvVulnerabilityAlerts: true` is the operative vulnerability-alert
   mechanism for this fork. No Dependabot re-enable required.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep CodeQL config as-is (broken) | No change | Silent misconfiguration; Go not scanned | Unacceptable security gap |
| Add Go to existing codeql-cpp job | One fewer job | Go requires a separate `init` step; mixing would break language detection | Not supported by CodeQL Action |
| Re-enable Dependabot alongside Renovate | Belt-and-suspenders | Duplicate PRs (ADR-0363 explicitly disabled it); paid-plan gate in any case | Renovate OSV alerts are sufficient |

## Consequences

- **Positive**: Go code in `cmd/`, `pkg/`, and `api/` is now scanned by
  CodeQL on every PR and weekly schedule. The CodeQL config is syntactically
  valid, so all existing C/C++/Python/Actions jobs now consume the correct
  path filters.
- **Negative**: One additional CI job (codeql-go, ~15 min) per PR touching
  Go files.
- **Neutral / follow-ups**: Once Rust bindings under `bindings/rust/` reach
  meaningful size, a `codeql-rust` job should be added (tracking CodeQL Rust
  GA, which was in beta as of 2026-05-29).

## References

- ADR-0700: `libvmaf/ → core/` rename.
- ADR-0363: Renovate supersedes Dependabot.
- ADR-0708: C++20 internals pilot (context for Phase 4 language
  modernization).
- `.github/workflows/security-scans.yml`, `.github/codeql-config.yml`.
- GitHub CodeQL docs — supported languages:
  <https://docs.github.com/en/code-security/code-scanning>
