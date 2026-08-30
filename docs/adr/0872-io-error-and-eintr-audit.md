<!-- markdownlint-disable MD060 -->
# ADR-0872: POSIX I/O EINTR-retry + return-value audit on fork-added C

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: `correctness`, `mcp`, `posix`, `nasa-power-of-10`

## Context

CLAUDE.md §6 mandates Power-of-10 rule 7: "every non-void return value
is checked or explicitly `(void)`-discarded." Two related concerns
combine to make POSIX I/O the highest-risk area for silent violations:

1. **EINTR semantics.** A blocking POSIX call (`read`, `write`, `close`,
   `accept`, ...) can return `-1` with `errno == EINTR` when a signal
   interrupts the kernel wait. Code that does not retry on EINTR treats
   the spurious wake-up as a hard error or as end-of-stream, silently
   losing data or corrupting stream framing.
2. **Discarded return values.** Even on best-effort cleanup paths,
   leaving the return naked of a `(void)` cast hides intent and trips
   clang-tidy / cppcheck pickier configurations, and erodes the rule
   by precedent.

The fork's MCP transports (`stdio`, `uds`, `sse`) and `compute_vmaf`
already had hardened `read_line` / `write_all` / `read_exact` helpers
that did the EINTR retry + partial-read/write loop correctly. But two
*secondary* drain loops (the "skip to next LF" path used when a
JSON-RPC request line exceeds the 64 KiB bound) did **not** retry on
EINTR — under signal pressure they could exit on a spurious `EINTR`
and leave the rest of the over-long line in the kernel buffer,
desynchronising the next request boundary.

Seven `close(2)` call sites scattered across `core/src/libvmaf.c`,
`core/src/feature/cambi.c`, `core/src/sycl/dmabuf_import.cpp`, and
`core/tools/vmaf_vpl.c` discarded the return value without the
`(void)` cast that rule 7 demands.

## Decision

We will:

1. **Retry on EINTR in the two MCP drain loops** so a signal-interrupted
   single-byte `read(2)` resumes instead of bailing. Pattern:

   ```c
   ssize_t r = read(fd, &c, 1);
   if (r < 0) {
       if (errno == EINTR) continue;
       break;
   }
   if (r == 0 || c == '\n') break;
   ```

   The previous combined `r <= 0` test conflated EOF (legitimate end-
   of-loop) with EINTR (must retry) with hard I/O error (must bail).
2. **Mark every fork-added `close(2)` call site that intentionally
   discards the return with an explicit `(void)` cast.** These are all
   cleanup or fail-path closures where we cannot meaningfully act on
   `EBADF` / `EIO` — but the cast signals intent and silences
   clang-tidy's `cert-err33-c`.

No silent data-loss EINTR-violations exist in the primary I/O paths
(`read_line`, `write_all_with_newline`, `sse_read_n`, `read_exact` all
already implement the textbook pattern). No partial-write defects
exist either — every `write_all*` helper loops until `off == len`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave drain-loop EINTR unhandled | Zero change | Silent stream desync under signal pressure — exact class of bug rule 7 is designed to prevent | Rejected: defeats the rule's intent |
| Wrap `close(2)` in a helper macro `vmaf_close()` that logs on failure | One choke point | All current sites are cleanup paths where logging would add noise without actionable signal; would require new header churn | Rejected: cost > benefit for ~7 sites |
| Audit only the MCP surface (skip the 7 close calls) | Smaller blast radius | Leaves rule-7 violations in tree visible to clang-tidy / next maintainer; rule applies project-wide | Rejected: half-measure |
| Convert drain loops to `MSG_PEEK` + bulk `recv` | Fewer syscalls per skipped byte | MSG_PEEK is socket-only and the stdio transport reads from a pipe / tty; would diverge the two transports' implementations | Rejected: keeps the cross-transport mirroring discipline already in tree |

## Consequences

- **Positive**: MCP transports survive signal-rich environments
  (containerised runtimes with periodic SIGCHLD / SIGURG, debuggers
  attaching) without silently corrupting the next request. Rule 7 stays
  clean on every fork-added C surface that uses `close(2)`.
- **Negative**: A few extra `(void)` casts add visual weight but cost
  nothing at runtime. The drain loop EINTR retry could in theory loop
  indefinitely under a pathological signal storm; in practice the
  loop is still bounded by the caller's read budget (the line is
  capped at `VMAF_MCP_MAX_LINE_BYTES`, so even if every byte triggers
  one EINTR retry the total iterations are bounded).
- **Neutral / follow-ups**: The vendored `core/src/svm.cpp:2522`
  `close(fd)` on the libsvm-rewrite fail path was left untouched — it
  is upstream libsvm code that the fork patches but does not own;
  per CLAUDE.md "vendored is in scope" the fork still has the right
  to fix it, but the cost (touching a long-stable port-only file for
  a fail-path cast) outweighs the benefit. Flagged for a future
  libsvm rebase pass.

## References

- [CLAUDE.md §6](../../CLAUDE.md) — Power-of-10 rule 7 ("every non-void
  return value is checked or explicitly `(void)`-discarded").
- [docs/principles.md](../principles.md) — NASA/JPL Power of 10
  applicable subset.
- [ADR-0278](0278-nolint-citation-closeout.md) — NOLINT citation
  closeout precedent for rule-7 hygiene.
- POSIX.1-2017 `read(2)`, `write(2)`, `close(2)`, `accept(2)` — EINTR
  semantics.
- Source: `req` (direct user direction, audit prompt 2026-05-30:
  "Audit C/C++ I/O calls for proper EINTR handling + missing error
  checks.").
