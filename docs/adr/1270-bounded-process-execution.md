# ADR-1270: Bound repository subprocess execution behind one validated API

- **Status**: Accepted
- **Date**: 2026-09-20
- **Deciders**: Kilian, Codex
- **Tags**: ci, security, tooling, python, fork-local

## Context

Repository automation had accumulated direct `subprocess.run`,
`check_output`, and `Popen` calls. Most were fixed argument vectors, but their
`# noqa: S603` annotations only told the linter to trust a call site. They did
not impose an executable allowlist, input and argument bounds, an output-memory
ceiling, a deadline, or descendant cleanup. The calls also differed in how
they handled missing tools, inherited Git state, non-UTF-8 output, and child
failures.

The warning annotations therefore described intent without enforcing it. A
reviewer had to re-prove the same safety properties at every call site, and a
later edit could invalidate the annotation without changing the lint result.

## Decision

Add the standard-library-only `scripts.lib.safe_subprocess` boundary and route
repository automation under `scripts/ci`, `scripts/dev`, `scripts/docs`,
`scripts/git-hooks`, `scripts/githooks`, and `scripts/lib` through it.

Every launch must provide an executable allowlist. The boundary resolves the
selected executable, rejects relative path traversal and NULs, bounds each
argument and the aggregate argument vector, validates the environment and
working directory, closes stdin unless bounded input is supplied, and applies
an explicit deadline. Captured stdout and stderr share a caller-configurable
memory ceiling. POSIX children run in a new session so timeout and overflow
cleanup signal the complete process group; Windows children run in a new
process group and use the platform termination path.

Expose synchronous and asynchronous entry points with immutable binary and
text result types plus explicit exception types. An uncaptured stream is the
empty value of its selected type; callers therefore never weaken successful
output handling to an unchecked nullable union. A stream redirected to a file
is outside the in-memory cap by design; the caller owns that file's retention
and size policy. Keep domain-level runner injection seams where tests need
them, but the production default inside the repository-automation scope
remains this boundary.

Make `scripts/` an explicit Python package and import the boundary only as
`scripts.lib.safe_subprocess`. Direct-path scripts prepend their resolved
repository root before that import. This gives the helper one runtime and mypy
module identity instead of the conflicting `lib.safe_subprocess` and
`scripts.lib.safe_subprocess` names exposed by the first pre-push run.

## Alternatives considered

| Option | Benefit | Cost or reason rejected |
| --- | --- | --- |
| Central validated boundary | Runtime enforcement and one adversarial test surface | Chosen; callers must translate standard-library keyword names explicitly |
| Keep direct calls plus `S603` annotations | Smallest diff | Annotation is not enforcement; timeout, output, and descendant behavior continue to drift |
| One wrapper per subsystem | Preserves local return types | Repeats security logic and makes fixes incomplete by construction |
| Shell wrappers | Familiar timeouts and redirection | Reintroduces quoting, injection, and platform divergence |

## Consequences

- **Positive**: the migrated automation has no executable direct
  `subprocess` launch left and no `S603` waiver is needed for those calls.
- **Positive**: floods, hangs, invalid executables, malformed arguments, and
  non-zero exits have executable negative controls.
- **Positive**: direct script execution and package imports share one helper
  class identity, and the complete changed Python scope is strict-mypy clean.
- **Negative**: callers must select realistic timeout and output ceilings;
  an undersized value fails closed and requires evidence before adjustment.
- **Neutral / follow-ups**: application packages with injectable runner APIs
  are outside this first automation boundary and need package-specific
  migrations rather than a signature-breaking mechanical replacement.

## References

- `req`: "there is no on touch rule anymore, no fucking warning or error is
  just ignored because of being og netflix code, fix them all ffs".
- [ADR-0525](0525-aiutils-subprocess-dedup.md): earlier package-local
  subprocess deduplication.
- [ADR-1241](1241-worktree-hook-dispatch.md): Git-hook command-execution
  requirements that this boundary now enforces.
- [Research-2071](../research/2071-bounded-process-execution.md): inventory,
  threat model, and validation evidence.
