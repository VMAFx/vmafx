<!-- markdownlint-disable MD060 -->
# ADR-0382: Y4M header parser — reject non-positive width or height before allocation

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `security`, `fuzz`, `parser`, `fork-local`

## Context

The nightly `fuzz.yml` `fuzz_y4m_input` job reported an
AddressSanitizer SEGV on address `0x000000000000` inside `fread`,
called from `y4m_input_fetch_frame` (`core/tools/y4m_input.c`),
for every scheduled run from 2026-05-05 through 2026-05-08
(T-FUZZ-Y4M-NEG-WIDTH-SEGV). The reproducer header
`YUV4MPEG2 W-8 H4 F30:1 Ip A1:1 C422` passes the tag scanner
(`y4m_parse_tags`) because `sscanf(p+1, "%d", &_y4m->pic_w)` accepts
negative decimal literals. The subsequent size arithmetic in
`y4m_input_open_impl` wraps or yields a non-positive product, so
`malloc` either receives a huge wrapped size and returns NULL, or
receives zero and returns a non-NULL stub that is too small.
Either way `_y4m->dst_buf` is unusable, yet `y4m_input_fetch_frame`
calls `fread(_y4m->dst_buf, 1, _y4m->dst_buf_read_sz, _fin)`
unconditionally — producing the NULL-deref SEGV.

This crash is distinct from the Y4M-411-OOB heap-buffer-overflow
fixed by PR #357 / commit `05ba29a6` at line 507 of
`y4m_convert_411_422jpeg`. That fix added bounds checks inside a
conversion routine; this fix gates the entire open path on valid
dimensions before any allocation occurs.

## Decision

Add a dimension validation guard in `y4m_input_open_impl`, immediately
after `y4m_parse_tags` returns successfully and before the chroma-type
dispatch block. If `pic_w <= 0` or `pic_h <= 0`, print a diagnostic
that includes the rejected values and return `-1`. No allocation is
attempted; the caller (`y4m_input_open`) propagates the error and
frees the outer struct.

The guard is placed at the earliest safe point: after the tags are
parsed (so the values are available) and before any arithmetic that
uses them (so no wrap-around can reach `malloc`). This satisfies
NASA/JPL Power of 10 rule 7 (check the return value / validity of
every data-dependent computation before acting on it) and SEI CERT C
INT32-C (avoid signed integer overflow in size arithmetic).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add guard in `y4m_parse_tags` directly at the `case 'W'` / `case 'H'` sites | Fails early, inside the scanner | Splits the "semantic validation" concern from "syntactic parsing"; `sscanf` may legitimately return a negative value for signed tag fields in other contexts | Rejected — cleaner to keep the scanner as a pure lexer and validate semantics at the open-impl level |
| Guard `malloc` result and skip `fread` when `dst_buf == NULL` | Avoids the SEGV without touching the parser | Masks the root cause; a NULL `dst_buf` with non-zero `dst_buf_read_sz` is never valid; any future code path could re-introduce the dereference | Rejected — defensive allocation-return checks do not substitute for input validation |
| Catch SIGSEGV with a signal handler in the fuzz harness | Prevents the fuzzer from crashing | Signal handlers are unsafe in ASAN mode; silences a real bug rather than fixing it | Rejected explicitly per task constraint |

## Consequences

- **Positive**: The fuzz harness no longer crashes on negative-dimension
  inputs; the nightly `fuzz_y4m_input` CI leg returns to green.
  The error message prints the rejected W/H values so operators can
  identify malformed sources in production logs.
- **Positive**: The reproducer byte sequence is promoted to
  `core/test/fuzz/y4m_input_known_crashes/y4m_neg_width_null_deref.y4m`
  — a permanent regression seed that libFuzzer replays on every run.
- **Negative**: Callers that previously received a SEGV now receive a
  clean `-1` error return and a `stderr` message. This is a strict
  improvement; no valid Y4M source has W=0 or W<0.
- **Neutral**: The fix touches only `y4m_input_open_impl` (an internal
  static function) — no public API change, no ffmpeg-patches impact,
  no rebase-notes entry needed.

## References

- T-FUZZ-Y4M-NEG-WIDTH-SEGV row in `docs/state.md`.
- ADR-0332 (`docs/adr/0332-nightly-fuzz-triage-keep-gates.md`) — policy
  to keep the fuzz workflow on while bugs are open.
- Prior Y4M parser fix: PR #357 / commit `05ba29a6`
  (`y4m_convert_411_422jpeg` 1-byte heap-buffer-overflow, ADR-0228).
- Crash artefact: `gh run download 25538384046 --repo VMAFx/vmafx
  --name fuzz_y4m_input-crashes-25538384046`; sha
  `crash-645a8f241b71d80ff496c10984d9b493d03dbfe1`.
- req: "Fix T-FUZZ-Y4M-NEG-WIDTH-SEGV: real ASan-detected SEGV in the
  Y4M parser ... reject W <= 0 / H <= 0 in the YUV4MPEG header parser
  before any allocation."
