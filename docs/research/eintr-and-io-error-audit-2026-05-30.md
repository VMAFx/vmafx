<!-- markdownlint-disable MD060 -->
# Research: POSIX I/O EINTR + return-value audit (2026-05-30)

**Companion to:** [ADR-0872](../adr/0872-io-error-and-eintr-audit.md)
**Scope:** Every fork-added C/C++ translation unit under `core/src/` and
`core/tools/` that calls any of `read`, `write`, `close`, `fsync`,
`fdatasync`, `select`, `pselect`, `nanosleep`, `sigtimedwait`, `accept`,
`connect`. Vendored sources (`third_party/`, `iqa/`, `3rdparty/cJSON`,
`libsvm`, `pdjson`) excluded except where the fork actively patches them.

## Methodology

1. Enumerate I/O call sites with
   `grep -rnE '\b(read|write|close|...)\s*\(' core/src core/tools` filtered
   against the vendored allowlist.
2. For each hit classify into one of four severity bins:
   - **HIGH**: silent data loss on EINTR (no retry) or partial
     write/read without a loop.
   - **MEDIUM**: return value discarded without explicit `(void)` cast
     (Power-of-10 rule 7 violation).
   - **LOW**: error handled but at wrong severity / wrong sink.
   - **NONE**: textbook retry + loop pattern already in place.
3. Read the surrounding context (5-line window) to confirm the bin
   (e.g. a `close()` inside a fail-cleanup branch is MEDIUM not HIGH).

## Findings

| Bin | Count | Sites |
|---|---|---|
| HIGH | 0 | — primary I/O helpers (`read_line`, `write_all_with_newline`, `sse_read_n`, `sse_write_all`, `read_exact`) already retry on EINTR and loop partial r/w correctly |
| MEDIUM (EINTR-unsafe drain) | 2 | `core/src/mcp/transport_stdio.c:150-156`, `core/src/mcp/transport_uds.c:134-139` — line-too-long drain loop did not retry on EINTR |
| MEDIUM (discarded `close(2)` return) | 7 | `core/src/libvmaf.c:2846`, `core/src/feature/cambi.c:739`, `core/src/sycl/dmabuf_import.cpp:323`, `core/src/sycl/dmabuf_import.cpp:350`, `core/tools/vmaf_vpl.c:117`, `core/tools/vmaf_vpl.c:136`, `core/tools/vmaf_vpl.c:145` |
| LOW | 0 | — |
| Deferred (vendored) | 1 | `core/src/svm.cpp:2522` (libsvm port; left for future libsvm rebase) |

## Fix shape

- **Drain-loop EINTR**: replace the combined `r <= 0` test with three
  branches — `r < 0 && errno == EINTR` (continue), `r < 0` (break),
  `r == 0 || c == '\n'` (break). Pattern matches the primary
  `read_line` helper already in tree.
- **`close(2)` casts**: insert `(void)` prefix at the call site.
  No behavioural change — these are all cleanup or fail-path branches
  where the caller could not act on the return.

## Validation

- Build: `meson setup build-eintr core -Denable_cuda=false -Denable_sycl=false`
  → `ninja -C build-eintr` clean.
- Fast tests: `meson test -C build-eintr --suite=fast --no-rebuild`
  → 49/49 OK including all MCP-transport unit tests.

## Out of scope

- File-descriptor-leak audit (FDs leaked on error paths). Adjacent
  concern but orthogonal; tracked separately.
- `pthread_*` return-value audit (rule 7 applies but is not POSIX-I/O).
- The C library `fopen` / `fwrite` family — these set `ferror()`
  internally and the fork's callers check via `fflush` / `fclose`
  return values, which the existing fast tests cover.
