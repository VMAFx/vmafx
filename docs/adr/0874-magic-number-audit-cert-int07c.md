<!-- markdownlint-disable MD013 MD060 -->
# ADR-0874: Name magic numbers in fork-added C surfaces (CERT INT07-C closeout pass 1)

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude (agent)
- **Tags**: cleanup, cert, mcp, dnn, picture, cuda

## Context

CERT C INT07-C and MISRA C:2012 rule 4.10 discourage unnamed numeric
literals in non-trivial contexts. Buffer caps, dimension bounds,
listener backlogs, bit-depth predicates, and protocol poll intervals
should carry semantic names rather than open-coded integers — the
reviewer should not have to reverse-engineer "why 16?" from
context.

A sweep of the fork-added C surfaces (`core/src/mcp/`,
`core/src/dnn/`, `core/src/cuda/`, `core/src/picture.c`,
`core/src/libvmaf.c`) found ~20 such literals where renaming
materially improves auditability without any behavioural change.
The CUDA, HIP, Metal, and SYCL backend runtimes were largely clean
already (their constants live under named `#define`s).

## Decision

Add named `#define` constants for the highest-impact magic numbers
in the touched files and rewrite call sites in terms of those
names. **No numeric value changes** — this is a rename-only pass
to preserve every existing invariant (including the
Netflix-golden-data gate).

Per-file additions:

- `core/src/mcp/mcp_internal.h` — `VMAF_MCP_LISTEN_BACKLOG`,
  `VMAF_MCP_TRANSPORT_BITMASK_MAX`,
  `VMAF_MCP_MAX_DRAIN_PER_FRAME`, `VMAF_MCP_SSE_PATH_MAX`,
  `VMAF_MCP_UDS_PATH_MAX`.
- `core/src/mcp/compute_vmaf.c` — `VMAF_MCP_PIC_DIM_{MIN,MAX}`,
  `VMAF_MCP_BITDEPTH_{8,10,12,16}`.
- `core/src/mcp/transport_sse.c` — `VMAF_MCP_SSE_STATUS_BUF`,
  `VMAF_MCP_SSE_LINE_HEADROOM`, `VMAF_MCP_SSE_FIELD_SCAN_BUDGET`,
  `VMAF_MCP_SSE_POLL_USEC`, `VMAF_MCP_SSE_DRAIN_BUF`,
  `VMAF_MCP_SSE_METHOD_BUF`, `VMAF_MCP_SSE_DECIMAL_RADIX`.
- `core/src/picture.c` — `VMAF_PIC_BPC_{MIN,MAX}`,
  `VMAF_PIC_DIM_MAX`.
- `core/src/cuda/picture_cuda.c` — `VMAF_CUDA_PIC_BPC_{MIN,MAX}`.
- `core/src/libvmaf.c` — `VMAF_DNN_NAME_FALLBACK_BUF`,
  `VMAF_DNN_NAME_DEDUP_BUF`, `VMAF_DNN_NAME_STRNLEN_CAP`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Sweep every literal across all backends in one PR | Single audit closeout | ~3.5K candidates surfaced by grep; majority are float-to-int IEEE-754 shifts (`0x7f800000`), well-known math constants, or already explained by surrounding ADR comments. Bundling them blows the PR LOC budget and dilutes review focus | Pass-1 closeout on the highest-signal files only; subsequent passes can chase remaining hotspots |
| `static const` instead of `#define` | Type-checked, debugger-visible | The values are used as array sizes (`char hdr[256]`) which require integral constant expressions in C99; `static const int` is not an ICE on some toolchains | `#define` keeps every site usable as array dim, switch case, and preprocessor branch |
| Group constants by domain in a new shared header | Cross-TU reuse | Most constants here are TU-local (per-transport SSE buffers, per-extractor DNN-name caps). Adding a header would create artificial coupling | Keep constants near point-of-use; only escalate to `mcp_internal.h` when shared across MCP TUs |
| Defer until a behavioural change touches the same files | Less churn | The CLAUDE.md §12 r12 touched-file cleanup rule already mandates lint-clean refactors when files are edited; doing the rename now lowers the bar for future PRs | Pre-emptively pay the audit cost while files are otherwise quiet |

## Consequences

- **Positive**: every numeric literal in the touched code now carries an
  inline explanation of *what* (semantic meaning) and *why* (derivation).
  Future grep-based audits skip these correctly; no `// NOLINT` needed.
- **Positive**: cross-platform AF_UNIX path cap (`VMAF_MCP_UDS_PATH_MAX`)
  is now discoverable from `mcp_internal.h` rather than buried as
  `>= 100u` inside `vmaf_mcp_start_uds`. Same for the listen backlog.
- **Negative**: every new include of `mcp_internal.h` pulls 5 extra
  macros into the TU namespace. All are `VMAF_MCP_*`-prefixed; namespace
  collision risk is negligible.
- **Neutral / follow-ups**: the bit-exact CPU golden gate is preserved
  (no numeric values changed). The CUDA / HIP / SYCL backends were
  largely clean already; only CUDA `picture_cuda.c`'s bit-depth
  predicate was renamed. Future passes can extend the audit to the
  remaining DNN tensor-io IEEE-754 shifts if a maintainer demands a
  fuller close-out.

## References

- CERT C INT07-C "Use only explicitly signed or unsigned char type for numerical values".
- MISRA C:2012 rule 4.10 (named-constant style).
- [`docs/principles.md`](../principles.md) — §1.2 rule 30 (banned functions),
  §2 (NASA/JPL Power of 10).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file cleanup invariant.
- [ADR-0278](0278-nolint-citation-closeout.md) — citation rigour.
- Source: `req` (direct user request, 2026-05-30) — paraphrased: sweep fork-added C code for magic numbers and hardcoded constants per CERT INT07-C and MISRA C:2012 rule 4.10.
