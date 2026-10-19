<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1899: govulncheck at symbol level, OpenVEX for what is not called

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: Lusoris
- **Tags**: security, dependencies, go, ci, fork-local

## Context

No CI step checked the Go module against the Go vulnerability database. The
dependency dashboard reported GO-2026-5932 against `golang.org/x/crypto`
v0.57.0. That advisory covers the deprecated `openpgp` packages and has no
fixed version. vmafx imports none of them: `go list -deps ./...` names no
`openpgp` package, and govulncheck v1.8.0 at symbol level reports the advisory
at module level only.

The same build graph held two other `x/crypto` packages that no vmafx code
needs:

- `md4`, through `golusoris/notify` → `wneessen/go-mail` → its NTLM SMTP
  authentication;
- `argon2`, through `golusoris/core/crypto` → `alexedwards/argon2id`.

Both arrived because three files imported the golusoris root package for the
`HTTP` and `Core` bundles. That package imports every module it bundles, so
each binary linked notify, crypto, the job queue, storage, search and the rest.

## Decision

- The `Go CI` workflow and `make govulncheck` run
  `scripts/ci/govulncheck-gate.py`. It runs govulncheck (`GOVULNCHECK_VERSION`
  in `build-config.env`, through `go run`) with `-scan symbol -format json` and
  judges each advisory by its most specific finding:
  - a called symbol fails the gate;
  - a finding at package or module level fails unless
    `security/vex/go.openvex.json` holds a `not_affected` statement for that
    advisory;
  - a `vulnerable_code_not_present` or `component_not_present` statement covers
    a module-level finding only, so once a package of the advisory is imported
    the gate fails again;
  - a scan that does not complete is exit 2.
- GO-2026-5932 gets a `not_affected` statement (`vulnerable_code_not_present`)
  with the evidence above.
- The golusoris modules vmafx uses are composed in `internal/app/bootstrap`
  (`bootstrap.Core`, `bootstrap.HTTP`) instead of being taken from the root
  package. This removes md4, argon2, go-mail, argon2id and 57 other modules
  from the build (a separate pull request).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| govulncheck at module level, failing on any advisory | Simplest rule | Fails on GO-2026-5932, which has no fix and is not compiled in; the only way out would be an ignore list without reasons | A recorded justification per advisory keeps the reason next to the exception |
| govulncheck at symbol level, ignoring package- and module-level findings | No VEX to maintain | An imported vulnerable package that is not called yet goes unrecorded; nobody revisits it when a call appears | The gate makes each uncalled advisory carry a statement |
| A third-party scanner instead of govulncheck | Covers several ecosystems | Reachability analysis is govulncheck's; others report at module level | govulncheck is the Go project's own tool and supports symbol-level scans |
| Remove md4 by changing golusoris (split its root package) | Fixes every golusoris consumer | A change in another repository and a release before vmafx benefits | Composing the modules locally is a three-file change with the same effect for vmafx; golusoris can follow on its own schedule |
| Keep md4 and argon2 and record them | No code change | The build graph would keep about 60 unused modules, NTLM among them | The dependencies are avoidable, so they go |

## Consequences

- **Positive**:
  - A reachable vulnerable Go symbol fails CI.
  - Every uncalled advisory carries a justification that the gate checks
    against the scan.
  - vmafx binaries no longer link the unused golusoris modules.
- **Negative**:
  - The gate needs network access (module download, vulnerability database)
    and a C compiler for cgo's type information; it links nothing.
  - Each new advisory that is not called needs a statement before the gate
    passes.
- **Neutral / follow-ups**:
  - Renovate moves `GOVULNCHECK_VERSION` through a custom manager.
  - When golusoris splits its root package, `bootstrap.Core` and
    `bootstrap.HTTP` can return to its bundles.

## References

- Maintainer decision relayed by the orchestrator on 2026-10-05 (paraphrased):
  add an OpenVEX statement for GO-2026-5932 and a pinned symbol-level
  govulncheck gate with a planted-defect proof, and trace which dependencies
  pull `x/crypto/md4` and `argon2`; replace them when avoidable.
- GO-2026-5932: <https://pkg.go.dev/vuln/GO-2026-5932>.
- govulncheck: <https://pkg.go.dev/golang.org/x/vuln/cmd/govulncheck> (v1.8.0);
  JSON output protocol v1.0.0.
- OpenVEX specification v0.2.0: <https://github.com/openvex/spec/blob/main/OPENVEX-SPEC.md>.
- [ADR-1886](1886-torch-training-environments-only.md) (the triage page and the
  OpenVEX document test this ADR reuses).
