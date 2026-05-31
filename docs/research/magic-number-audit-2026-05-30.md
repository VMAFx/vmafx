<!-- markdownlint-disable MD060 -->
# Magic-number audit (fork-added C surfaces) — 2026-05-30

## Scope

Sweep fork-added C code for unnamed numeric literals that have semantic
meaning (CERT C INT07-C, MISRA C:2012 rule 4.10).

## Method

1. Enumerate suspicious literals with the deferred-tool grep pattern
   `\b[0-9]{2,}[uUlL]?\b` in `core/src/**.c` and `core/src/**.h`, excluding
   third-party / iqa / 3rdparty trees and copyright headers.
2. Triage by fork-added directory:

   | Directory | Raw literal count (post-exclusions) |
   |---|---|
   | `core/src/dnn` | 152 |
   | `core/src/mcp` | 80 |
   | `core/src/cuda` | 30 |
   | `core/src/hip` | 16 |
   | `core/src/metal` | 11 |
   | `core/src/sycl` | 5 |

3. Re-triage to drop:
   - IEEE-754 / float16 bit-field shifts (`0x7f800000u`, `<< 23`) — these
     are width-encoded constants, not magic numbers; the comment header on
     each function already documents the IEEE-754 reference.
   - JSON-RPC standard error codes (already in a named enum in `dispatcher.c`).
   - In-tree constants that are already symbolic (`VMAF_CUDA_DRAIN_BATCH_MAX`,
     `VMAF_CUDA_THREADS_PER_WARP`, `VMAF_MCP_SSE_BODY_MAX`, etc.).
   - Buffer caps already explained by an inline comment that derives
     the value from a sibling constant (e.g. `cap = VMAF_MCP_MAX_LINE_BYTES + 256u`).

4. Promote the highest-impact residue to named `#define`s.

## Findings (pass 1)

| File | Literal | New name | Reason kept after triage |
|---|---|---|---|
| `core/src/mcp/mcp.c` | `listen(fd, 16)` × 2 | `VMAF_MCP_LISTEN_BACKLOG` | Cross-cutting SOMAXCONN approximation; visible in two TUs |
| `core/src/mcp/mcp.c` | `(unsigned)transport > 31u` × 2 | `VMAF_MCP_TRANSPORT_BITMASK_MAX` | Encodes the implicit `sizeof(unsigned) * CHAR_BIT - 1` shift safety per CERT INT34-C |
| `core/src/mcp/mcp.c` | `cfg->max_drain_per_frame > 64u` | `VMAF_MCP_MAX_DRAIN_PER_FRAME` | Public config-validation predicate; reviewer needs to know "why 64" |
| `core/src/mcp/mcp.c` | `len > 256u` (SSE path) | `VMAF_MCP_SSE_PATH_MAX` | HTTP-route cap |
| `core/src/mcp/mcp.c` | `path_len >= 100u` (UDS) | `VMAF_MCP_UDS_PATH_MAX` | Cross-POSIX `sun_path` headroom |
| `core/src/mcp/compute_vmaf.c` | `wv < 8.0 \|\| ... > 8192.0` | `VMAF_MCP_PIC_DIM_{MIN,MAX}` | YUV dimension bounds for the MCP tool surface |
| `core/src/mcp/compute_vmaf.c` | `bitdepth in {8,10,12,16}` | `VMAF_MCP_BITDEPTH_{8,10,12,16}` | Pixel-format whitelist |
| `core/src/mcp/transport_sse.c` | `char hdr[256]` | `VMAF_MCP_SSE_STATUS_BUF` | HTTP-status scratch |
| `core/src/mcp/transport_sse.c` | `cap = LINE + 256u` | `VMAF_MCP_SSE_LINE_HEADROOM` | Document the headroom delta |
| `core/src/mcp/transport_sse.c` | `budget = 64u` | `VMAF_MCP_SSE_FIELD_SCAN_BUDGET` | Bounded-scan safety |
| `core/src/mcp/transport_sse.c` | `tv_usec = 200 * 1000` | `VMAF_MCP_SSE_POLL_USEC` | Worker heartbeat |
| `core/src/mcp/transport_sse.c` | `char drain[64]` | `VMAF_MCP_SSE_DRAIN_BUF` | Inbound discard scratch |
| `core/src/mcp/transport_sse.c` | `char method[16]` | `VMAF_MCP_SSE_METHOD_BUF` | HTTP-method scratch |
| `core/src/mcp/transport_sse.c` | `v = v * 10 + ...` | `VMAF_MCP_SSE_DECIMAL_RADIX` | Decimal-radix annotation |
| `core/src/picture.c` | `bpc < 8 \|\| bpc > 16` | `VMAF_PIC_BPC_{MIN,MAX}` | Cross-backend bit-depth bound |
| `core/src/picture.c` | `w > 32768u \|\| h > 32768u` | `VMAF_PIC_DIM_MAX` | CERT INT30-C derivation |
| `core/src/cuda/picture_cuda.c` | `bpc < 8 \|\| bpc > 16` × 2 | `VMAF_CUDA_PIC_BPC_{MIN,MAX}` | Same as picture.c, kept local to TU |
| `core/src/libvmaf.c` | `char fallback[32]` | `VMAF_DNN_NAME_FALLBACK_BUF` | Documented derivation from `output%zu` worst case |
| `core/src/libvmaf.c` | `char suffix[48]` | `VMAF_DNN_NAME_DEDUP_BUF` | Documented dedup-loop suffix bound |
| `core/src/libvmaf.c` | `strnlen(*, 1024u)` × 2 | `VMAF_DNN_NAME_STRNLEN_CAP` | CERT STR06-C cap |

Total: 26 call-site rewrites, ~20 named constants.

## Out of scope (pass 2+ candidates)

- `core/src/dnn/tensor_io.c` IEEE-754 bit shifts (`0x7f800000`, `<< 23`,
  `0x1fu`, `>> 13`). These encode the IEEE-754 binary32/binary16 layout
  directly; renaming them risks obscuring the standard reference and would
  need an `IEEE754_*` header that none of the touched TUs share. Defer to a
  dedicated float16 ADR if maintainers want a tighter close-out.
- `predict.c` percentile / bootstrap statistics literals (`2.5`, `97.5`,
  `0.01`). These are upstream Netflix code; renaming would create an
  unnecessary rebase delta against `upstream/master`.
- `mcp/dispatcher.c` JSON-RPC error codes — already in a named enum.

## Verification

- CPU-only build: `meson setup build-magic core -Denable_cuda=false
  -Denable_sycl=false && ninja -C build-magic` — clean.
- Unit tests: `meson test -C build-magic` — 63/63 pass.
- Bit-exactness: no numeric values changed; Netflix golden gate
  preserved by construction.
- `scripts/ci/assertion-density.sh`: pass (161 asserts across 99 fork
  functions).

## References

- CERT C INT07-C, INT30-C, INT34-C, STR06-C.
- MISRA C:2012 rule 4.10.
- ADR-0874.
