<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-0983: gosec sweep — fix all findings + add CI gate

- **Status**: Accepted
- **Date**: 2026-06-01
- **Deciders**: lusoris
- **Tags**: security, ci, go

## Context

The Go surface area on the fork has grown to roughly 11k lines across the
controller, node, server, operator, MCP, tune, and supporting `pkg/`
libraries, but no automated static-security scan was running against it.
`gosec` is the standard SAST tool for Go and integrates with the existing
`go-ci.yml` workflow. The user requested a single bundled fix-up pass:
run gosec, fix every finding, gate the surface from regressing.

The audit produced 38 raw findings. Six G115 cgo integer-cast warnings and
four G103 `unsafe.Slice` warnings live in `gen/go/*.pb.go` (protoc-generated)
and the cgo trampoline cache for `pkg/libvmaf` — both populate via
`go generate` / cgo and are not maintainable by hand. The remaining 26 are
in fork-original source.

One finding was a real bug: `cmd/vmafx-mcp/impl.go::describeModel`
unconditionally joined the caller-supplied `name` argument onto the repo
root and called `os.Stat` / `os.ReadFile`. A request such as
`{"name": "../../../etc/passwd"}` would have reached `/etc/passwd` because
the candidate path was not constrained to `libvmaf.AllowedRoots()`. This
ADR documents the fix.

The other 25 source findings were verified false positives (constant
binary names, paths that round-trip through `libvmaf.ValidatePath`,
`os.CreateTemp` outputs) but had been suppressed via
`//nolint:gosec` — gosec does not parse golangci-lint nolint directives,
so the suppressions were ignored. The sweep rewrites every suppression to
gosec's `// #nosec G<rule>` form with the citation required by
CLAUDE.md §12 r12 (NOLINT citation rule, extended in spirit to `#nosec`).

## Decision

We will:

1. Rewrite every `//nolint:gosec ...` suppression to
   `// #nosec G<rule>` with an inline citation that names the rule, the
   reason it is a verified false positive, and the upstream validation
   that justifies the suppression.
2. Fix the one real bug (`describeModel` path traversal) by routing the
   candidate path through `libvmaf.ValidatePath` before opening.
3. Tighten file permissions in `cmd/vmafx-tune/cmd/compare.go::writeOutput`
   to `0o600` (file) / `0o750` (directory) per G306 / G301.
4. Handle the previously-ignored `outFile.Close()` and `os.Remove` errors
   in `runVmafScore`.
5. Add a `lint-go` target to the Makefile that invokes
   `gosec -exclude-generated -quiet ./...` and wire the same step into
   `.github/workflows/go-ci.yml` so future PRs gate at zero findings
   outside `gen/`.
6. Add a regression test
   (`cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal`)
   that asserts `describeModel` rejects path-traversal-style names.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `//nolint:gosec` and skip the gate | No code churn | gosec ignores those directives — findings would have re-surfaced on every fresh scan and any new finding would have been buried in the existing noise | The point of the gate is regression-prevention; ignored suppressions defeat the gate |
| Use a project-level `.gosec.yaml` exclusion file | One central knob | Hides the why-it-is-safe rationale from the call site; violates the CLAUDE.md §12 r12 citation requirement | Inline citation is the established pattern (mirrors the C `// NOLINT(...)` discipline) |
| Skip the generated-file findings via SARIF post-filter | Allows gating on every finding including generated noise | Adds tooling for no security benefit — the cgo / protobuf casts are bounded by their generators | `-exclude-generated` is already a first-class gosec flag |
| Drop the gate entirely after this sweep | Smallest diff | Trades a 30-second CI step for a recurring multi-hour audit burden; the next round of growth re-fills the queue | The user's explicit request was to bundle every finding into one PR with an implicit "and don't regress" |

## Consequences

- **Positive**: gosec runs on every push touching `**/*.go`; new findings
  surface immediately as a red required check rather than waiting for an
  ad-hoc audit. `describeModel` is now path-traversal-safe.
- **Negative**: every new subprocess call or file open in `pkg/` or `cmd/`
  must either pass through a validating helper or add a `// #nosec`
  citation. This matches the existing C cleanup discipline (NOLINT
  citation rule), so the cognitive overhead is small.
- **Neutral / follow-ups**: the cgo cache findings (G115 in
  `pkg/libvmaf/direct.go`) and protobuf-generated G103 findings remain
  outside the gate via `-exclude-generated`. If we later want to police
  them, the right place is a `gen/go/AGENTS.md` invariant note plus a
  one-shot scan after each `go generate`.

## References

- Source: req (user direction to "Run `gosec` security scanner across
  the entire Go codebase + fix ALL findings. Bundle into ONE DRAFT PR",
  paraphrased to remove imperative-mood profanity).
- CLAUDE.md §12 r12 — NOLINT citation rule, extended in spirit to
  `// #nosec` directives.
- [ADR-0278](0278-nolint-citation-closeout.md) — citation closeout
  precedent for clang-tidy NOLINT comments.
- [docs/research/gosec-findings-fix-sweep-2026-06-01.md](../research/gosec-findings-fix-sweep-2026-06-01.md)
  — per-finding fix rationale.
- Upstream: <https://github.com/securego/gosec>.
