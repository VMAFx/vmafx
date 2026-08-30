<!-- markdownlint-disable MD013 MD060 -->
# ADR-0977: core/tools input-reader safety — Y4M malloc-NULL check, YUV/Y4M size_t cast, bench GPU-state leaks

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `security`, `bug`, `tools`, `audit`

## Context

A deep-dive audit of `core/tools/` surfaces (CLI binary + bench binary +
the vendored Daala YUV / Y4M readers) found three real defects:

1. **`y4m_input_open_impl` ignored `malloc()` return values** (lines
   815-816). `dst_buf = malloc(dst_buf_sz)` and the conditional `aux_buf =
   malloc(aux_buf_sz)` both stored NULL into the reader struct on OOM and
   the function still returned 0. The caller chain (`y4m_input_open` →
   `video_input_open`) then surfaced the reader as a valid `video_input`.
   The very next `video_input_fetch_frame()` called
   `fread(_y4m->dst_buf, …)` with `dst_buf == NULL`, faulting inside
   libc as a SIGSEGV. Reachable by any header that demands a buffer the
   process can't allocate — practical via `ulimit -v` or against an
   attacker-controlled (or fuzzer-controlled) header that declares
   absurdly-large dimensions.

2. **`y4m_input.c` + `yuv_input.c` computed `dst_buf_sz` in `int` /
   `unsigned` precision** (lines 804 / 162-174 respectively). Both files
   then assigned the result to a `size_t` field, where the high bits had
   already wrapped for headers near the 32-bit ceiling. `malloc` then
   succeeded with a wrap-to-small size and the first `fread` overflowed
   the heap allocation. The 4:4:4 paths in `y4m_input.c` already cast to
   `size_t` (lines 736 / 743 / 779), proving the precedent.

3. **`vmaf_bench.c::bench_feature` leaked `VmafCudaState` /
   `VmafSyclState`** on every success and on most error paths.
   `cu_state` / `sycl_state` were declared local to their `#ifdef`
   blocks, so `vmaf_*_state_free` could never be called from later
   exit paths. The companion routine `run_feature_collect` in the
   same file was already fixed under the T5 state-leak audit
   (2026-05-30) and carries the hoist-and-free pattern explicitly in
   a code comment. `bench_feature` was not updated at the same time.

The audit deliberately did **not** modify any Netflix golden-data
assertion (CLAUDE.md §8) and did not weaken any test threshold or
gate.

## Decision

Apply three coordinated fixes in one PR:

- **Y4M malloc-NULL check**: `y4m_input_open_impl` now returns -1 when
  `malloc()` fails, freeing any partial allocation. `y4m_input_open`
  zero-initialises the struct before calling impl so dst_buf/aux_buf
  are NULL on any other early-return path.
- **size_t-precision buffer sizing**: cast pic_w / pic_h (Y4M:
  signed int) and width / height (YUV: unsigned) to `size_t` before
  the multiplication. The intermediate arithmetic now proceeds in
  `size_t` precision on every supported 64-bit host, matching the
  pattern the 4:4:4 paths in `y4m_input.c` already follow.
- **bench_feature GPU-state lifetime**: hoist `cu_state` / `sycl_state`
  to function scope and route every exit through a single
  `bench_cleanup` label that runs the per-state `_state_free` plus
  `vmaf_close` plus `yuv_pair_close` in reverse-init order.

A new regression test `test_y4m_alloc_failure` drives the parser
against a header that declares 65535×65535 4:4:4 12-bit (≈ 25 GiB
of buffer), with `setrlimit(RLIMIT_AS)` clamping address space below
the required size — forcing the `malloc` inside `y4m_input_open_impl`
to fail deterministically. Pre-fix the test fails (parser reports
success then would crash on fetch); post-fix the test passes
(parser rejects open with -1, no crash).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix Y4M malloc-NULL only, defer overflow & bench leaks | Smaller surface, easier review | Three independent surfaces share the same root-cause (insufficient defensive coding around `malloc` + size arithmetic in vendored input parsers); splitting fragments the fix story and re-opens two of three holes on the next pass | Bundle matches the per-CLAUDE-feedback "bigger PRs over fragmented micro-PRs" preference; all three live in `core/tools/` |
| Add an upper-bound check on pic_w/pic_h in the Y4M parser | Direct defence-in-depth against the absurd-dimension case | Doesn't address the underlying `int` overflow; a smaller header that still overflows when multiplied by chroma+bitdepth survives | Necessary upper bounds are a separate hardening pass; the size_t cast removes the wrap regardless of what the header declares |
| Replace the vendored Daala parser entirely | Eliminates the whole class of bugs | Out of scope; the parser is upstream-mirrored and any rewrite needs its own ADR + research digest | Out of scope; this ADR is a targeted defect-fix |
| `// NOLINT` the affected lines | Zero behavioural change | Hides a real reachable crash | Violates CLAUDE.md §12 r12 (NOLINT only for load-bearing invariants) |

## Consequences

- **Positive**: A malformed (or attacker-controlled) Y4M header with
  absurd dimensions no longer crashes the CLI; it now surfaces a clean
  error and a non-zero exit. The benchmark binary no longer leaks GPU
  state per invocation (every `bench_feature` call previously leaked
  the SYCL / CUDA state struct + its underlying USM / device buffers).
- **Negative**: `bench_feature` gained a `goto bench_cleanup` cleanup
  label. The function is slightly longer.
- **Neutral / follow-ups**: One regression test added
  (`test_y4m_alloc_failure`, fast suite, POSIX-only). The pre-existing
  CLI-fuzz harness exercise increases (the new error path is now
  fuzz-reachable as a clean rejection rather than as a SIGSEGV).

## References

- Source: bug-audit task brief, 2026-05-31 session.
- Companion ADR for the pattern: T5 state-leak audit comment in
  `core/tools/vmaf_bench.c::run_feature_collect` (2026-05-30).
- Related: ADR-0316 (cli_parse banned-function audit),
  ADR-0438 (atoi removal in vmaf_bench), test_y4m_411_oob.c
  (precedent for fmemopen-based parser regression tests).
